/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zenith/cpu.h>
#include <zenith/xstate.h>

#include "../src/kernel/xstate_runtime.h"

#define TEST_CR0_MP (UINT64_C(1) << 1U)
#define TEST_CR0_EM (UINT64_C(1) << 2U)
#define TEST_CR0_TS (UINT64_C(1) << 3U)
#define TEST_CR0_NE (UINT64_C(1) << 5U)
#define TEST_CR4_REQUIRED ((UINT64_C(1) << 9U) | (UINT64_C(1) << 10U) | \
    (UINT64_C(1) << 18U))

static uint64_t test_cr0 = TEST_CR0_EM | TEST_CR0_TS;
static uint64_t test_cr4;
static uint64_t test_xcr0 = XSTATE_X87_MASK;
static size_t save_calls;
static size_t restore_calls;
static bool test_interrupts = true;
static bool corrupt_next_save;

uint64_t xstate_arch_read_cr4(void);
void xstate_arch_write_cr4(uint64_t value);
uint64_t xstate_arch_xgetbv(uint32_t register_index);
void xstate_arch_xsetbv(uint32_t register_index, uint64_t value);
void xstate_arch_save(void *area, uint64_t mask);
void xstate_arch_restore(const void *area, uint64_t mask);

void cpu_interrupt_disable(void)
{
    test_interrupts = false;
}

void cpu_interrupt_enable(void)
{
    test_interrupts = true;
}

bool cpu_interrupts_enabled(void)
{
    return test_interrupts;
}

uint64_t cpu_read_cr0(void)
{
    return test_cr0;
}

void cpu_write_cr0(uint64_t value)
{
    test_cr0 = value;
}

void cpu_cpuid(
    uint32_t leaf,
    uint32_t subleaf,
    struct cpu_cpuid_result *result
)
{
    result->eax = 0U;
    result->ebx = 0U;
    result->ecx = 0U;
    result->edx = 0U;
    if (leaf == 0U) {
        result->eax = UINT32_C(0xD);
    } else if (leaf == 1U) {
        result->ecx = (UINT32_C(1) << 26U) |
            (UINT32_C(1) << 27U) | (UINT32_C(1) << 28U);
        result->edx = (UINT32_C(1) << 0U) |
            (UINT32_C(1) << 24U) | (UINT32_C(1) << 25U) |
            (UINT32_C(1) << 26U);
    } else if (leaf == UINT32_C(0xD) && subleaf == 0U) {
        result->eax = (uint32_t)XSTATE_BOUNDED_MASK;
        result->ebx = UINT32_C(832);
        result->ecx = UINT32_C(832);
    } else if (leaf == UINT32_C(0xD) && subleaf == 2U) {
        result->eax = UINT32_C(256);
        result->ebx = XSTATE_MINIMUM_AREA_SIZE;
    }
}

uint64_t xstate_arch_read_cr4(void)
{
    return test_cr4;
}

void xstate_arch_write_cr4(uint64_t value)
{
    test_cr4 = value;
}

uint64_t xstate_arch_xgetbv(uint32_t register_index)
{
    return register_index == 0U ? test_xcr0 : 0U;
}

void xstate_arch_xsetbv(uint32_t register_index, uint64_t value)
{
    if (register_index == 0U) {
        test_xcr0 = value;
    }
}

void xstate_arch_save(void *area, uint64_t mask)
{
    uint8_t *bytes = (uint8_t *)area;

    ++save_calls;
    if (corrupt_next_save) {
        bytes[XSTATE_LEGACY_AREA_SIZE + 7U] = UINT8_C(0x80);
        corrupt_next_save = false;
    }
    (void)mask;
}

void xstate_arch_restore(const void *area, uint64_t mask)
{
    ++restore_calls;
    (void)area;
    (void)mask;
}

static bool status_is(enum xstate_status observed, enum xstate_status expected)
{
    if (observed != expected) {
        fprintf(stderr, "xstate status %d, expected %d\n",
            (int)observed, (int)expected);
        return false;
    }
    return true;
}

int main(int argument_count, char **arguments)
{
    struct xstate_task_handle handles[XSTATE_TASK_LIMIT];
    struct xstate_task_handle overflow;
    struct xstate_runtime_snapshot snapshot;
    const struct xstate_task_handle bootstrap = {
        XSTATE_INVALID_INDEX,
        0U
    };
    const bool unexpected_nm_scenario =
        argument_count == 2 && strcmp(arguments[1], "unexpected-nm") == 0;

    if (argument_count > 2 ||
        (argument_count == 2 && !unexpected_nm_scenario)) {
        return 2;
    }

    if (!status_is(xstate_runtime_initialize(), XSTATE_STATUS_OK) ||
        !test_interrupts ||
        (test_cr0 & (TEST_CR0_MP | TEST_CR0_NE)) !=
            (TEST_CR0_MP | TEST_CR0_NE) ||
        (test_cr0 & (TEST_CR0_EM | TEST_CR0_TS)) != 0U ||
        (test_cr4 & TEST_CR4_REQUIRED) != TEST_CR4_REQUIRED ||
        test_xcr0 != XSTATE_BOUNDED_MASK || save_calls != 1U ||
        !status_is(
            xstate_runtime_initialize(),
            XSTATE_STATUS_ALREADY_INITIALIZED
        )) {
        return 1;
    }

    for (size_t index = 0U; index < XSTATE_TASK_LIMIT; ++index) {
        if (!status_is(
                xstate_runtime_task_create(&handles[index]),
                XSTATE_STATUS_OK
            ) || handles[index].index != index ||
            handles[index].generation != 1U) {
            return 1;
        }
    }
    if (!status_is(
            xstate_runtime_task_create(&overflow),
            XSTATE_STATUS_NO_TASK_SLOT
        ) || overflow.index != XSTATE_INVALID_INDEX ||
        !status_is(
            xstate_runtime_snapshot(&snapshot),
            XSTATE_STATUS_OK
        ) || snapshot.live_tasks != XSTATE_TASK_LIMIT ||
        !snapshot.bootstrap_loaded) {
        return 1;
    }

    cpu_interrupt_disable();
    if (!status_is(
            xstate_runtime_switch(true, bootstrap, false, handles[0]),
            XSTATE_STATUS_OK
        )) {
        return 1;
    }
    for (size_t index = 1U; index < XSTATE_TASK_LIMIT; ++index) {
        if (!status_is(
                xstate_runtime_switch(
                    false,
                    handles[index - 1U],
                    false,
                    handles[index]
                ),
                XSTATE_STATUS_OK
            )) {
            return 1;
        }
    }
    if (!status_is(
            xstate_runtime_switch(
                false,
                handles[XSTATE_TASK_LIMIT - 1U],
                true,
                bootstrap
            ),
            XSTATE_STATUS_OK
        ) || save_calls != XSTATE_TASK_LIMIT + 2U ||
        restore_calls != XSTATE_TASK_LIMIT + 1U) {
        return 1;
    }
    cpu_interrupt_enable();

    for (size_t index = 0U; index < XSTATE_TASK_LIMIT; ++index) {
        if (!status_is(
                xstate_runtime_task_destroy(handles[index]),
                XSTATE_STATUS_OK
            )) {
            return 1;
        }
    }
    if (!status_is(
            xstate_runtime_snapshot(&snapshot),
            XSTATE_STATUS_OK
        ) || snapshot.live_tasks != 0U || !snapshot.bootstrap_loaded ||
        !status_is(
            xstate_runtime_task_validate(handles[0], false),
            XSTATE_STATUS_STALE_HANDLE
        ) || !status_is(
            xstate_runtime_task_create(&handles[0]),
            XSTATE_STATUS_OK
        )) {
        return 1;
    }

    cpu_interrupt_disable();
    if (unexpected_nm_scenario) {
        if (!status_is(
                xstate_runtime_unexpected_nm(),
                XSTATE_STATUS_UNEXPECTED_NM
            ) || xstate_runtime_is_initialized() ||
            !status_is(
                xstate_runtime_validate(),
                XSTATE_STATUS_POISONED
            )) {
            return 1;
        }

        puts("xstate runtime unexpected #NM poison check passed");
        return 0;
    }

    corrupt_next_save = true;
    if (!status_is(
            xstate_runtime_switch(true, bootstrap, false, handles[0]),
            XSTATE_STATUS_OK
        ) || !status_is(
            xstate_runtime_switch(false, handles[0], true, bootstrap),
            XSTATE_STATUS_CORRUPTED
        ) || xstate_runtime_is_initialized()) {
        return 1;
    }

    puts("xstate runtime capacity, ownership, and poison checks passed");
    return 0;
}
