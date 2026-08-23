/* SPDX-License-Identifier: GPL-3.0-only */
/* One measured Linux x86-64 SYSCALL subset for the BusyBox echo proof. */

#include <sapote/linux_syscall.h>

#include <stddef.h>

#include <sapote/console.h>
#include <sapote/cpu.h>
#include <sapote/linux_abi.h>
#include <sapote/memory.h>

#define CPUID_EXTENDED_ROOT UINT32_C(0x80000000)
#define CPUID_EXTENDED_FEATURES UINT32_C(0x80000001)
#define CPUID_SYSCALL_SYSRET (UINT32_C(1) << 11U)
#define IA32_EFER UINT32_C(0xC0000080)
#define IA32_STAR UINT32_C(0xC0000081)
#define IA32_LSTAR UINT32_C(0xC0000082)
#define IA32_FMASK UINT32_C(0xC0000084)
#define IA32_FS_BASE UINT32_C(0xC0000100)
#define EFER_SCE UINT64_C(1)
#define ARCH_SET_FS UINT64_C(0x1002)
#define LINUX_STAR_VALUE \
    ((UINT64_C(0x23) << 48U) | (UINT64_C(0x08) << 32U))
#define LINUX_FMASK_VALUE \
    ((UINT64_C(1) << 8U) | (UINT64_C(1) << 9U) | \
        (UINT64_C(1) << 10U) | (UINT64_C(1) << 14U) | \
        (UINT64_C(1) << 18U))
#define LINUX_USER_RFLAGS_ALLOWED UINT64_C(0x8D7)
#define LINUX_ERRNO_EINVAL 22
#define LINUX_ERRNO_ENOSYS 38
#define LINUX_KERNEL_STACK_BYTES (16U * 1024U)

_Static_assert(sizeof(struct linux_syscall_frame) == 144U,
    "Linux syscall assembly frame size changed");
_Static_assert(offsetof(struct linux_syscall_frame, rax) == 0U &&
    offsetof(struct linux_syscall_frame, rdi) == 8U &&
    offsetof(struct linux_syscall_frame, r10) == 32U &&
    offsetof(struct linux_syscall_frame, r9) == 48U &&
    offsetof(struct linux_syscall_frame, rip) == 104U &&
    offsetof(struct linux_syscall_frame, rflags) == 120U &&
    offsetof(struct linux_syscall_frame, rsp) == 128U &&
    offsetof(struct linux_syscall_frame, ss) == 136U,
    "Linux syscall assembly frame offsets changed");
_Static_assert(LINUX_SYSCALL_ALLOWLIST_COUNT <= LINUX_SYSCALL_ALLOWLIST_MAX,
    "Linux syscall allowlist exceeds the fixed ceiling");

enum stdout_sink_state {
    STDOUT_SINK_CANDIDATE = 0,
    STDOUT_SINK_ARMED,
    STDOUT_SINK_WRITTEN,
    STDOUT_SINK_RELEASED
};

enum provenance_state {
    PROVENANCE_CANDIDATE = 0,
    PROVENANCE_ENTERED,
    PROVENANCE_COMPLETED,
    PROVENANCE_RELEASED
};

struct linux_syscall_runtime {
    struct linux_syscall_context context;
    struct linux_syscall_result result;
    uint64_t saved_efer;
    uint64_t saved_star;
    uint64_t saved_lstar;
    uint64_t saved_fmask;
    uint64_t saved_fs_base;
    uint64_t request_generation;
    uint32_t call_index;
    uint32_t request_ordinal;
    enum linux_syscall_cpu_state state;
    enum stdout_sink_state stdout_state;
    enum provenance_state provenance_state;
    bool seen[LINUX_SYSCALL_ALLOWLIST_COUNT];
    bool heap_mapped[PAGING_LINUX_HEAP_PAGES];
    bool anonymous_mapped;
    bool active;
};

static const uint64_t allowlist[LINUX_SYSCALL_ALLOWLIST_COUNT] = {
    UINT64_C(1), UINT64_C(9), UINT64_C(11), UINT64_C(12),
    UINT64_C(158), UINT64_C(218), UINT64_C(231)
};

static const uint64_t expected_calls[LINUX_SYSCALL_EXPECTED_CALLS] = {
    UINT64_C(158), UINT64_C(218), UINT64_C(12), UINT64_C(12),
    UINT64_C(9), UINT64_C(9), UINT64_C(1), UINT64_C(11), UINT64_C(231)
};

static const uint8_t expected_stdout[LINUX_SYSCALL_STDOUT_BYTES] = {
    'S', 'A', 'P', 'O', 'T', 'E', '\n'
};

static struct linux_syscall_runtime runtime;

/* Loaded by the second instruction in linux_syscall_entry. */
uint64_t linux_syscall_kernel_stack;

static void zero_bytes(void *pointer, size_t length)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool canonical_user(uint64_t address)
{
    return address <= UINT64_C(0x00007FFFFFFFFFFF);
}

static bool user_range_shape_valid(uint64_t address, size_t length)
{
    return length != 0U && canonical_user(address) &&
        address <= UINT64_MAX - (uint64_t)(length - 1U) &&
        canonical_user(address + (uint64_t)(length - 1U));
}

static bool syscall_supported(void)
{
    struct cpuid_result root;
    struct cpuid_result features;

    cpu_cpuid(CPUID_EXTENDED_ROOT, 0U, &root);
    if (root.eax < CPUID_EXTENDED_FEATURES) {
        return false;
    }
    cpu_cpuid(CPUID_EXTENDED_FEATURES, 0U, &features);
    return (features.edx & CPUID_SYSCALL_SYSRET) != 0U;
}

static enum linux_syscall_status transition_cpu(
    enum linux_syscall_cpu_state next
)
{
    bool allowed = false;

    if (next >= LINUX_SYSCALL_CPU_STATE_COUNT || runtime.state == next) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    switch (runtime.state) {
    case LINUX_SYSCALL_CPU_CANDIDATE:
        allowed = next == LINUX_SYSCALL_CPU_ARMED;
        break;
    case LINUX_SYSCALL_CPU_ARMED:
        allowed = next == LINUX_SYSCALL_CPU_ENTERED ||
            next == LINUX_SYSCALL_CPU_DISARMED;
        break;
    case LINUX_SYSCALL_CPU_ENTERED:
        allowed = next == LINUX_SYSCALL_CPU_RETURNED;
        break;
    case LINUX_SYSCALL_CPU_RETURNED:
        allowed = next == LINUX_SYSCALL_CPU_ENTERED ||
            next == LINUX_SYSCALL_CPU_DISARMED;
        break;
    case LINUX_SYSCALL_CPU_DISARMED:
    case LINUX_SYSCALL_CPU_STATE_COUNT:
        break;
    }
    if (!allowed) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    runtime.state = next;
    runtime.result.cpu_state = next;
    return LINUX_SYSCALL_STATUS_OK;
}

static enum linux_syscall_status transition_stdout(enum stdout_sink_state next)
{
    bool allowed = false;

    if (next > STDOUT_SINK_RELEASED || runtime.stdout_state == next) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    switch (runtime.stdout_state) {
    case STDOUT_SINK_CANDIDATE:
        allowed = next == STDOUT_SINK_ARMED ||
            next == STDOUT_SINK_RELEASED;
        break;
    case STDOUT_SINK_ARMED:
        allowed = next == STDOUT_SINK_WRITTEN ||
            next == STDOUT_SINK_RELEASED;
        break;
    case STDOUT_SINK_WRITTEN:
        allowed = next == STDOUT_SINK_RELEASED;
        break;
    case STDOUT_SINK_RELEASED:
        break;
    }
    if (!allowed) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    runtime.stdout_state = next;
    return LINUX_SYSCALL_STATUS_OK;
}

static enum linux_syscall_status transition_provenance(
    enum provenance_state next
)
{
    bool allowed = false;

    if (next > PROVENANCE_RELEASED || runtime.provenance_state == next) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    switch (runtime.provenance_state) {
    case PROVENANCE_CANDIDATE:
        allowed = next == PROVENANCE_ENTERED ||
            next == PROVENANCE_RELEASED;
        break;
    case PROVENANCE_ENTERED:
        allowed = next == PROVENANCE_COMPLETED ||
            next == PROVENANCE_RELEASED;
        break;
    case PROVENANCE_COMPLETED:
        allowed = next == PROVENANCE_CANDIDATE ||
            next == PROVENANCE_RELEASED;
        break;
    case PROVENANCE_RELEASED:
        break;
    }
    if (!allowed) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    runtime.provenance_state = next;
    return LINUX_SYSCALL_STATUS_OK;
}

static size_t allowlist_index(uint64_t number)
{
    for (size_t index = 0U; index < LINUX_SYSCALL_ALLOWLIST_COUNT; ++index) {
        if (allowlist[index] == number) {
            return index;
        }
    }
    return SIZE_MAX;
}

static bool enosys_result(uint64_t number, uint64_t *result)
{
    if (result == NULL || allowlist_index(number) != SIZE_MAX) {
        return false;
    }
    *result = (uint64_t)(int64_t)-LINUX_ERRNO_ENOSYS;
    return true;
}

static bool context_equal(
    const struct linux_syscall_context *left,
    const struct linux_syscall_context *right
)
{
    if (left->address_space != right->address_space ||
        left->process_generation != right->process_generation ||
        left->executable_start != right->executable_start ||
        left->executable_end != right->executable_end ||
        left->stack_start != right->stack_start ||
        left->stack_end != right->stack_end ||
        left->fs_address != right->fs_address ||
        left->tid_address != right->tid_address ||
        left->anonymous_frame != right->anonymous_frame ||
        left->exit_observed != right->exit_observed ||
        left->failure_before_ordinal != right->failure_before_ordinal ||
        left->failure_after_ordinal != right->failure_after_ordinal ||
        left->controlled_run != right->controlled_run ||
        left->publish_stdout != right->publish_stdout) {
        return false;
    }
    for (size_t index = 0U; index < PAGING_LINUX_HEAP_PAGES; ++index) {
        if (left->heap_frames[index] != right->heap_frames[index]) {
            return false;
        }
    }
    return true;
}

static bool result_equal(
    const struct linux_syscall_result *left,
    const struct linux_syscall_result *right
)
{
    return left->syscall_count == right->syscall_count &&
        left->distinct_syscalls == right->distinct_syscalls &&
        left->stdout_bytes == right->stdout_bytes &&
        left->exit_status == right->exit_status &&
        left->status == right->status &&
        left->cpu_state == right->cpu_state &&
        left->stdout_valid == right->stdout_valid &&
        left->exit_zero == right->exit_zero &&
        left->real_syscall_instruction == right->real_syscall_instruction &&
        left->process_authenticated == right->process_authenticated &&
        left->cr3_authenticated == right->cr3_authenticated &&
        left->cpu_disarmed == right->cpu_disarmed &&
        left->controlled_failure_observed ==
            right->controlled_failure_observed;
}

static bool runtime_equal(
    const struct linux_syscall_runtime *left,
    const struct linux_syscall_runtime *right
)
{
    if (!context_equal(&left->context, &right->context) ||
        !result_equal(&left->result, &right->result) ||
        left->saved_efer != right->saved_efer ||
        left->saved_star != right->saved_star ||
        left->saved_lstar != right->saved_lstar ||
        left->saved_fmask != right->saved_fmask ||
        left->saved_fs_base != right->saved_fs_base ||
        left->request_generation != right->request_generation ||
        left->call_index != right->call_index ||
        left->request_ordinal != right->request_ordinal ||
        left->state != right->state ||
        left->stdout_state != right->stdout_state ||
        left->provenance_state != right->provenance_state ||
        left->anonymous_mapped != right->anonymous_mapped ||
        left->active != right->active) {
        return false;
    }
    for (size_t index = 0U; index < LINUX_SYSCALL_ALLOWLIST_COUNT; ++index) {
        if (left->seen[index] != right->seen[index]) {
            return false;
        }
    }
    for (size_t index = 0U; index < PAGING_LINUX_HEAP_PAGES; ++index) {
        if (left->heap_mapped[index] != right->heap_mapped[index]) {
            return false;
        }
    }
    return true;
}

static bool msr_values_valid(
    uint64_t efer,
    uint64_t star,
    uint64_t lstar,
    uint64_t fmask
)
{
    return (efer & EFER_SCE) != 0U && star == LINUX_STAR_VALUE &&
        lstar == (uint64_t)(uintptr_t)linux_syscall_entry &&
        fmask == LINUX_FMASK_VALUE;
}

static bool msr_contract_valid(void)
{
    return msr_values_valid(cpu_read_msr(IA32_EFER),
        cpu_read_msr(IA32_STAR), cpu_read_msr(IA32_LSTAR),
        cpu_read_msr(IA32_FMASK));
}

static bool user_writable(uint64_t address, size_t length)
{
    uint64_t cursor = address;
    size_t remaining = length;

    if (!user_range_shape_valid(address, length)) {
        return false;
    }
    while (remaining > 0U) {
        struct paging_translation translation;
        size_t chunk = (size_t)(PAGING_PAGE_SIZE -
            (cursor & (PAGING_PAGE_SIZE - 1U)));

        if (chunk > remaining) {
            chunk = remaining;
        }
        if (paging_process_translate(runtime.context.address_space, cursor,
                &translation) != PAGING_STATUS_OK || !translation.user ||
            (translation.permissions & PAGING_WRITE) == 0U ||
            translation.level != 1U ||
            !frame_range_overlaps_allocatable_memory(
                translation.physical_address, chunk)) {
            return false;
        }
        cursor += chunk;
        remaining -= chunk;
    }
    return true;
}

static bool copy_from_user(void *destination, uint64_t address, size_t length)
{
    uint8_t *output = destination;
    uint64_t cursor = address;
    size_t remaining = length;

    if (destination == NULL || !user_range_shape_valid(address, length)) {
        return false;
    }
    while (remaining > 0U) {
        struct paging_translation translation;
        size_t chunk = (size_t)(PAGING_PAGE_SIZE -
            (cursor & (PAGING_PAGE_SIZE - 1U)));

        if (chunk > remaining) {
            chunk = remaining;
        }
        if (paging_process_translate(runtime.context.address_space, cursor,
                &translation) != PAGING_STATUS_OK || !translation.user ||
            translation.level != 1U ||
            !frame_range_overlaps_allocatable_memory(
                translation.physical_address, chunk)) {
            return false;
        }
        const uint8_t *input =
            (const uint8_t *)(uintptr_t)translation.physical_address;

        for (size_t index = 0U; index < chunk; ++index) {
            output[index] = input[index];
        }
        output += chunk;
        cursor += chunk;
        remaining -= chunk;
    }
    return true;
}

static bool frame_return_shape_valid(
    const struct linux_syscall_context *context,
    const struct linux_syscall_frame *frame
)
{
    return context != NULL && frame != NULL &&
        context->executable_start <= UINT64_MAX - 2U &&
        context->executable_start + 2U < context->executable_end &&
        context->stack_start < context->stack_end &&
        frame->cs == CPU_GDT_USER_CODE_SELECTOR &&
        frame->ss == CPU_GDT_USER_DATA_SELECTOR &&
        (frame->rflags & UINT64_C(2)) != 0U &&
        (frame->rflags & ~LINUX_USER_RFLAGS_ALLOWED) == 0U &&
        canonical_user(frame->rip) &&
        frame->rip >= context->executable_start + 2U &&
        frame->rip < context->executable_end &&
        canonical_user(frame->rsp) && frame->rsp >= context->stack_start &&
        frame->rsp < context->stack_end;
}

static enum linux_syscall_status validate_entry(
    const struct linux_syscall_frame *frame
)
{
    const uintptr_t stack_pointer = cpu_read_stack_pointer();
    uint8_t instruction[2];

    if (frame == NULL) {
        return LINUX_SYSCALL_STATUS_NULL_ARGUMENT;
    }
    if (!runtime.active || runtime.context.address_space == NULL ||
        runtime.provenance_state != PROVENANCE_CANDIDATE ||
        (runtime.state != LINUX_SYSCALL_CPU_ARMED &&
            runtime.state != LINUX_SYSCALL_CPU_RETURNED)) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    if (runtime.context.process_generation == 0U ||
        runtime.request_generation != runtime.context.process_generation) {
        return LINUX_SYSCALL_STATUS_BAD_GENERATION;
    }
    if (runtime.context.address_space->state != PAGING_PROCESS_SPACE_ACTIVE ||
        (cpu_read_cr3() & ~(PAGING_PAGE_SIZE - 1U)) !=
            runtime.context.address_space->root_physical_address) {
        return LINUX_SYSCALL_STATUS_BAD_CR3;
    }
    if (cpu_read_cs() != CPU_GDT_CODE_SELECTOR ||
        linux_syscall_kernel_stack == 0U ||
        stack_pointer > linux_syscall_kernel_stack ||
        stack_pointer < linux_syscall_kernel_stack -
            LINUX_KERNEL_STACK_BYTES ||
        (uintptr_t)frame > linux_syscall_kernel_stack ||
        (uintptr_t)frame < linux_syscall_kernel_stack -
            LINUX_KERNEL_STACK_BYTES) {
        return LINUX_SYSCALL_STATUS_BAD_STACK;
    }
    if (!frame_return_shape_valid(&runtime.context, frame)) {
        return LINUX_SYSCALL_STATUS_BAD_RETURN;
    }
    if (!copy_from_user(instruction, frame->rip - 2U,
            sizeof(instruction)) || instruction[0] != UINT8_C(0x0F) ||
        instruction[1] != UINT8_C(0x05)) {
        return LINUX_SYSCALL_STATUS_BAD_ENTRY;
    }
    return LINUX_SYSCALL_STATUS_OK;
}

static uintptr_t return_to_kernel(enum linux_syscall_status status)
{
    uintptr_t resume_stack;

    runtime.result.status = status;
    if (runtime.state == LINUX_SYSCALL_CPU_ENTERED) {
        (void)transition_cpu(LINUX_SYSCALL_CPU_RETURNED);
    }
    if (runtime.context.address_space != NULL &&
        runtime.context.address_space->state == PAGING_PROCESS_SPACE_ACTIVE &&
        paging_process_restore_kernel(runtime.context.address_space) !=
            PAGING_STATUS_OK) {
        runtime.result.status = LINUX_SYSCALL_STATUS_RESTORE;
    }
    resume_stack = linux_process_resume_stack();
    return resume_stack;
}

bool linux_syscall_cpu_foundation_self_test(size_t *completed_tests)
{
    struct linux_syscall_runtime saved_runtime;

    if (completed_tests == NULL) {
        return false;
    }
    *completed_tests = 0U;
    if (!syscall_supported() || !cpu_user_transition_contract_valid() ||
        cpu_tss_rsp0() == 0U ||
        CPU_GDT_CODE_SELECTOR != UINT16_C(0x08) ||
        CPU_GDT_DATA_SELECTOR != UINT16_C(0x10) ||
        CPU_GDT_USER_DATA_SELECTOR != UINT16_C(0x2B) ||
        CPU_GDT_USER_CODE_SELECTOR != UINT16_C(0x33) ||
        LINUX_STAR_VALUE != ((UINT64_C(0x23) << 48U) |
            (UINT64_C(0x08) << 32U)) ||
        LINUX_FMASK_VALUE != UINT64_C(0x44700) ||
        LINUX_SYSCALL_ALLOWLIST_COUNT > LINUX_SYSCALL_ALLOWLIST_MAX ||
        sizeof(struct linux_syscall_frame) != 144U ||
        !msr_values_valid(EFER_SCE, LINUX_STAR_VALUE,
            (uint64_t)(uintptr_t)linux_syscall_entry,
            LINUX_FMASK_VALUE) ||
        msr_values_valid(0U, LINUX_STAR_VALUE,
            (uint64_t)(uintptr_t)linux_syscall_entry,
            LINUX_FMASK_VALUE) ||
        msr_values_valid(EFER_SCE, LINUX_STAR_VALUE ^ UINT64_C(1),
            (uint64_t)(uintptr_t)linux_syscall_entry,
            LINUX_FMASK_VALUE) ||
        msr_values_valid(EFER_SCE, LINUX_STAR_VALUE,
            UINT64_C(0x0000800000000000), LINUX_FMASK_VALUE) ||
        msr_values_valid(EFER_SCE, LINUX_STAR_VALUE,
            (uint64_t)(uintptr_t)linux_syscall_entry,
            LINUX_FMASK_VALUE ^ UINT64_C(1))) {
        return false;
    }
    if (runtime.active) {
        return false;
    }
    saved_runtime = runtime;
    zero_bytes(&runtime, sizeof(runtime));
    runtime.state = LINUX_SYSCALL_CPU_CANDIDATE;
    runtime.result.cpu_state = LINUX_SYSCALL_CPU_CANDIDATE;
    if (transition_cpu(LINUX_SYSCALL_CPU_ARMED) != LINUX_SYSCALL_STATUS_OK ||
        transition_cpu(LINUX_SYSCALL_CPU_ARMED) !=
            LINUX_SYSCALL_STATUS_BAD_STATE ||
        transition_cpu(LINUX_SYSCALL_CPU_ENTERED) !=
            LINUX_SYSCALL_STATUS_OK ||
        transition_cpu(LINUX_SYSCALL_CPU_DISARMED) !=
            LINUX_SYSCALL_STATUS_BAD_STATE ||
        transition_cpu(LINUX_SYSCALL_CPU_RETURNED) !=
            LINUX_SYSCALL_STATUS_OK ||
        transition_cpu(LINUX_SYSCALL_CPU_ENTERED) !=
            LINUX_SYSCALL_STATUS_OK ||
        transition_cpu(LINUX_SYSCALL_CPU_RETURNED) !=
            LINUX_SYSCALL_STATUS_OK ||
        transition_cpu(LINUX_SYSCALL_CPU_DISARMED) !=
            LINUX_SYSCALL_STATUS_OK ||
        transition_cpu(LINUX_SYSCALL_CPU_ENTERED) !=
            LINUX_SYSCALL_STATUS_BAD_STATE ||
        transition_stdout(STDOUT_SINK_ARMED) !=
            LINUX_SYSCALL_STATUS_OK ||
        transition_stdout(STDOUT_SINK_ARMED) !=
            LINUX_SYSCALL_STATUS_BAD_STATE ||
        transition_stdout(STDOUT_SINK_WRITTEN) !=
            LINUX_SYSCALL_STATUS_OK ||
        transition_stdout(STDOUT_SINK_ARMED) !=
            LINUX_SYSCALL_STATUS_BAD_STATE ||
        transition_stdout(STDOUT_SINK_RELEASED) !=
            LINUX_SYSCALL_STATUS_OK ||
        transition_stdout(STDOUT_SINK_WRITTEN) !=
            LINUX_SYSCALL_STATUS_BAD_STATE ||
        transition_provenance(PROVENANCE_ENTERED) !=
            LINUX_SYSCALL_STATUS_OK ||
        transition_provenance(PROVENANCE_ENTERED) !=
            LINUX_SYSCALL_STATUS_BAD_STATE ||
        transition_provenance(PROVENANCE_COMPLETED) !=
            LINUX_SYSCALL_STATUS_OK ||
        transition_provenance(PROVENANCE_CANDIDATE) !=
            LINUX_SYSCALL_STATUS_OK ||
        transition_provenance(PROVENANCE_RELEASED) !=
            LINUX_SYSCALL_STATUS_OK ||
        transition_provenance(PROVENANCE_ENTERED) !=
            LINUX_SYSCALL_STATUS_BAD_STATE) {
        runtime = saved_runtime;
        return false;
    }
    runtime = saved_runtime;
    *completed_tests = LINUX_SYSCALL_CPU_FOUNDATION_CONTROLS;
    return true;
}

bool linux_syscall_enosys_self_test(void)
{
    static const uint64_t refused[] = {
        UINT64_C(0), UINT64_C(2), UINT64_C(60), UINT64_C(999), UINT64_MAX
    };

    const struct linux_syscall_runtime before = runtime;

    for (size_t index = 0U; index < sizeof(refused) / sizeof(refused[0]);
         ++index) {
        uint64_t result = 0U;

        if (!enosys_result(refused[index], &result) ||
            (int64_t)result != -LINUX_ERRNO_ENOSYS) {
            return false;
        }
    }
    for (size_t index = 0U; index < LINUX_SYSCALL_ALLOWLIST_COUNT; ++index) {
        uint64_t result = 0U;

        if (enosys_result(allowlist[index], &result)) {
            return false;
        }
    }
    return runtime_equal(&before, &runtime);
}

enum linux_syscall_status linux_syscall_arm(
    const struct linux_syscall_context *context
)
{
    const uint64_t kernel_stack = (uint64_t)cpu_tss_rsp0();

    if (context == NULL || context->address_space == NULL) {
        return LINUX_SYSCALL_STATUS_NULL_ARGUMENT;
    }
    if (runtime.active || linux_process_boundary_active()) {
        return LINUX_SYSCALL_STATUS_BUSY;
    }
    if (!syscall_supported()) {
        return LINUX_SYSCALL_STATUS_UNSUPPORTED_CPU;
    }
    if (!cpu_user_transition_contract_valid() || kernel_stack == 0U ||
        cpu_interrupts_enabled() ||
        context->address_space->state != PAGING_PROCESS_SPACE_INSTALLED ||
        context->process_generation == 0U ||
        context->executable_start != LINUX_ABI_EXECUTABLE_START ||
        context->executable_end != LINUX_ABI_EXECUTABLE_END ||
        context->stack_start != PAGING_LINUX_STACK_BASE ||
        context->stack_end != PAGING_LINUX_STACK_END ||
        context->fs_address != LINUX_ABI_FS_ADDRESS ||
        context->tid_address != LINUX_ABI_TID_ADDRESS) {
        return LINUX_SYSCALL_STATUS_BAD_PROCESS;
    }
    for (size_t index = 0U; index < PAGING_LINUX_HEAP_PAGES; ++index) {
        if (context->heap_frames[index] == 0U) {
            return LINUX_SYSCALL_STATUS_BAD_PROCESS;
        }
    }
    if (context->anonymous_frame == 0U || context->exit_observed == NULL) {
        return LINUX_SYSCALL_STATUS_BAD_PROCESS;
    }
    if ((context->failure_before_ordinal != 0U &&
            context->failure_after_ordinal != 0U) ||
        context->failure_before_ordinal > LINUX_SYSCALL_EXPECTED_CALLS ||
        context->failure_after_ordinal > LINUX_SYSCALL_EXPECTED_CALLS ||
        context->controlled_run == context->publish_stdout ||
        ((context->failure_before_ordinal != 0U ||
            context->failure_after_ordinal != 0U) &&
                !context->controlled_run)) {
        return LINUX_SYSCALL_STATUS_BAD_PROCESS;
    }
    zero_bytes(&runtime, sizeof(runtime));
    runtime.context = *context;
    runtime.state = LINUX_SYSCALL_CPU_CANDIDATE;
    runtime.result.cpu_state = runtime.state;
    runtime.stdout_state = STDOUT_SINK_CANDIDATE;
    runtime.provenance_state = PROVENANCE_CANDIDATE;
    runtime.request_generation = context->process_generation;
    runtime.saved_efer = cpu_read_msr(IA32_EFER);
    runtime.saved_star = cpu_read_msr(IA32_STAR);
    runtime.saved_lstar = cpu_read_msr(IA32_LSTAR);
    runtime.saved_fmask = cpu_read_msr(IA32_FMASK);
    runtime.saved_fs_base = cpu_read_msr(IA32_FS_BASE);
    linux_syscall_kernel_stack = kernel_stack;
    cpu_write_msr(IA32_STAR, LINUX_STAR_VALUE);
    cpu_write_msr(IA32_LSTAR, (uint64_t)(uintptr_t)linux_syscall_entry);
    cpu_write_msr(IA32_FMASK, LINUX_FMASK_VALUE);
    cpu_write_msr(IA32_EFER, runtime.saved_efer | EFER_SCE);
    runtime.active = true;
    if (transition_cpu(LINUX_SYSCALL_CPU_ARMED) != LINUX_SYSCALL_STATUS_OK ||
        transition_stdout(STDOUT_SINK_ARMED) != LINUX_SYSCALL_STATUS_OK ||
        !msr_contract_valid()) {
        (void)linux_syscall_disarm();
        return LINUX_SYSCALL_STATUS_MSR_CONTRACT;
    }
    runtime.result.status = LINUX_SYSCALL_STATUS_OK;
    return LINUX_SYSCALL_STATUS_OK;
}

enum linux_syscall_status linux_syscall_validate_armed(void)
{
    if (!runtime.active || runtime.state != LINUX_SYSCALL_CPU_ARMED ||
        runtime.context.address_space == NULL) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    if (!msr_contract_valid() || linux_syscall_kernel_stack != cpu_tss_rsp0()) {
        return LINUX_SYSCALL_STATUS_MSR_CONTRACT;
    }
    return LINUX_SYSCALL_STATUS_OK;
}

static enum linux_syscall_status map_heap(void)
{
    for (size_t index = 0U; index < PAGING_LINUX_HEAP_PAGES; ++index) {
        if (paging_process_map_user_page(runtime.context.address_space,
                PAGING_PROCESS_MAPPING_LINUX_HEAP,
                PAGING_LINUX_HEAP_BASE + index * PAGING_PAGE_SIZE,
                runtime.context.heap_frames[index], PAGING_WRITE) !=
                PAGING_STATUS_OK) {
            for (size_t rollback = 0U; rollback < index; ++rollback) {
                (void)paging_process_unmap_user_page(
                    runtime.context.address_space,
                    PAGING_PROCESS_MAPPING_LINUX_HEAP,
                    PAGING_LINUX_HEAP_BASE + rollback * PAGING_PAGE_SIZE);
                runtime.heap_mapped[rollback] = false;
            }
            return LINUX_SYSCALL_STATUS_MAPPING;
        }
        runtime.heap_mapped[index] = true;
    }
    return LINUX_SYSCALL_STATUS_OK;
}

static bool arguments_match(
    const struct linux_syscall_runtime *candidate,
    size_t call_index,
    const struct linux_syscall_frame *frame
)
{
    if (candidate == NULL || frame == NULL) {
        return false;
    }
    switch (call_index) {
    case 0U:
        return frame->rdi == ARCH_SET_FS &&
            frame->rsi == candidate->context.fs_address;
    case 1U:
        return frame->rdi == candidate->context.tid_address;
    case 2U:
        return frame->rdi == 0U;
    case 3U:
        return frame->rdi == PAGING_LINUX_HEAP_BASE +
            PAGING_LINUX_HEAP_PAGES * PAGING_PAGE_SIZE;
    case 4U:
        return frame->rdi == PAGING_LINUX_HEAP_BASE &&
            frame->rsi == PAGING_PAGE_SIZE && frame->rdx == 0U &&
            frame->r10 == UINT64_C(0x32) && frame->r8 == UINT64_MAX &&
            frame->r9 == 0U && candidate->heap_mapped[0];
    case 5U:
        return frame->rdi == 0U && frame->rsi == PAGING_PAGE_SIZE &&
            frame->rdx == UINT64_C(3) &&
            frame->r10 == UINT64_C(0x22) && frame->r8 == UINT64_MAX &&
            frame->r9 == 0U && !candidate->anonymous_mapped;
    case 6U:
        return frame->rdi == 1U &&
            frame->rdx == LINUX_SYSCALL_STDOUT_BYTES &&
            candidate->stdout_state == STDOUT_SINK_ARMED;
    case 7U:
        return frame->rdi == PAGING_LINUX_ANON_ADDRESS &&
            frame->rsi == PAGING_PAGE_SIZE && candidate->anonymous_mapped;
    case 8U:
        return frame->rdi == 0U &&
            candidate->stdout_state == STDOUT_SINK_WRITTEN &&
            !candidate->anonymous_mapped;
    default:
        return false;
    }
}

static void expected_arguments(
    struct linux_syscall_runtime *candidate,
    size_t call_index,
    struct linux_syscall_frame *frame
)
{
    zero_bytes(candidate, sizeof(*candidate));
    zero_bytes(frame, sizeof(*frame));
    candidate->context.fs_address = LINUX_ABI_FS_ADDRESS;
    candidate->context.tid_address = LINUX_ABI_TID_ADDRESS;
    switch (call_index) {
    case 0U:
        frame->rdi = ARCH_SET_FS;
        frame->rsi = candidate->context.fs_address;
        break;
    case 1U:
        frame->rdi = candidate->context.tid_address;
        break;
    case 2U:
        break;
    case 3U:
        frame->rdi = PAGING_LINUX_HEAP_BASE +
            PAGING_LINUX_HEAP_PAGES * PAGING_PAGE_SIZE;
        break;
    case 4U:
        frame->rdi = PAGING_LINUX_HEAP_BASE;
        frame->rsi = PAGING_PAGE_SIZE;
        frame->r10 = UINT64_C(0x32);
        frame->r8 = UINT64_MAX;
        candidate->heap_mapped[0] = true;
        break;
    case 5U:
        frame->rsi = PAGING_PAGE_SIZE;
        frame->rdx = UINT64_C(3);
        frame->r10 = UINT64_C(0x22);
        frame->r8 = UINT64_MAX;
        break;
    case 6U:
        frame->rdi = 1U;
        frame->rdx = LINUX_SYSCALL_STDOUT_BYTES;
        candidate->stdout_state = STDOUT_SINK_ARMED;
        break;
    case 7U:
        frame->rdi = PAGING_LINUX_ANON_ADDRESS;
        frame->rsi = PAGING_PAGE_SIZE;
        candidate->anonymous_mapped = true;
        break;
    case 8U:
        candidate->stdout_state = STDOUT_SINK_WRITTEN;
        break;
    default:
        break;
    }
}

bool linux_syscall_semantic_self_test(void)
{
    struct linux_syscall_runtime candidate;
    struct linux_syscall_frame frame;

    for (size_t call = 0U; call < LINUX_SYSCALL_EXPECTED_CALLS; ++call) {
        struct linux_syscall_frame changed;

        expected_arguments(&candidate, call, &frame);
        if (!arguments_match(&candidate, call, &frame)) {
            return false;
        }
        changed = frame;
        changed.rdi ^= UINT64_C(1);
        if (arguments_match(&candidate, call, &changed)) {
            return false;
        }
        if (call == 0U || call == 4U || call == 5U || call == 7U) {
            changed = frame;
            changed.rsi ^= UINT64_C(1);
            if (arguments_match(&candidate, call, &changed)) {
                return false;
            }
        }
        if (call == 4U || call == 5U || call == 6U) {
            changed = frame;
            changed.rdx ^= UINT64_C(1);
            if (arguments_match(&candidate, call, &changed)) {
                return false;
            }
        }
        if (call == 4U || call == 5U) {
            changed = frame;
            changed.r10 ^= UINT64_C(1);
            if (arguments_match(&candidate, call, &changed)) {
                return false;
            }
            changed = frame;
            changed.r8 ^= UINT64_C(1);
            if (arguments_match(&candidate, call, &changed)) {
                return false;
            }
            changed = frame;
            changed.r9 ^= UINT64_C(1);
            if (arguments_match(&candidate, call, &changed)) {
                return false;
            }
        }
    }
    if (arguments_match(&candidate, LINUX_SYSCALL_EXPECTED_CALLS, &frame)) {
        return false;
    }
    for (size_t allowed = 0U; allowed < LINUX_SYSCALL_ALLOWLIST_COUNT;
         ++allowed) {
        bool seen = false;

        for (size_t call = 0U; call < LINUX_SYSCALL_EXPECTED_CALLS; ++call) {
            if (expected_calls[call] == allowlist[allowed]) {
                seen = true;
            }
        }
        if (!seen) {
            return false;
        }
    }
    zero_bytes(&candidate, sizeof(candidate));
    zero_bytes(&frame, sizeof(frame));
    candidate.context.executable_start = UINT64_C(0x400000);
    candidate.context.executable_end = UINT64_C(0x401000);
    candidate.context.stack_start = UINT64_C(0x500000);
    candidate.context.stack_end = UINT64_C(0x504000);
    frame.cs = CPU_GDT_USER_CODE_SELECTOR;
    frame.ss = CPU_GDT_USER_DATA_SELECTOR;
    frame.rflags = UINT64_C(2);
    frame.rip = candidate.context.executable_start + 2U;
    frame.rsp = candidate.context.stack_end - 8U;
    if (!frame_return_shape_valid(&candidate.context, &frame)) {
        return false;
    }
    frame.cs = CPU_GDT_CODE_SELECTOR;
    if (frame_return_shape_valid(&candidate.context, &frame)) {
        return false;
    }
    frame.cs = CPU_GDT_USER_CODE_SELECTOR;
    frame.ss = CPU_GDT_DATA_SELECTOR;
    if (frame_return_shape_valid(&candidate.context, &frame)) {
        return false;
    }
    frame.ss = CPU_GDT_USER_DATA_SELECTOR;
    frame.rflags = UINT64_C(0x1002);
    if (frame_return_shape_valid(&candidate.context, &frame)) {
        return false;
    }
    frame.rflags = UINT64_C(2);
    frame.rip = candidate.context.executable_end;
    if (frame_return_shape_valid(&candidate.context, &frame)) {
        return false;
    }
    frame.rip = candidate.context.executable_start + 2U;
    frame.rsp = candidate.context.stack_start - 1U;
    if (frame_return_shape_valid(&candidate.context, &frame)) {
        return false;
    }
    return LINUX_SYSCALL_ALLOWLIST_COUNT <= LINUX_SYSCALL_ALLOWLIST_MAX &&
        user_range_shape_valid(UINT64_C(0x1000), PAGING_PAGE_SIZE) &&
        !user_range_shape_valid(UINT64_C(0x1000), 0U) &&
        !user_range_shape_valid(UINT64_MAX, 2U) &&
        !user_range_shape_valid(UINT64_C(0x0000800000000000), 1U) &&
        !user_range_shape_valid(UINT64_C(0x00007FFFFFFFFFFF), 2U);
}

static enum linux_syscall_status execute_call(
    struct linux_syscall_frame *frame,
    uint64_t *return_value
)
{
    if (!arguments_match(&runtime, runtime.call_index, frame)) {
        return LINUX_SYSCALL_STATUS_BAD_ARGUMENT;
    }
    switch (runtime.call_index) {
    case 0U:
        if (!user_writable(frame->rsi, sizeof(uint64_t))) {
            return LINUX_SYSCALL_STATUS_BAD_ARGUMENT;
        }
        cpu_write_msr(IA32_FS_BASE, frame->rsi);
        if (cpu_read_msr(IA32_FS_BASE) != frame->rsi) {
            return LINUX_SYSCALL_STATUS_MSR_CONTRACT;
        }
        *return_value = 0U;
        return LINUX_SYSCALL_STATUS_OK;
    case 1U:
        if (!user_writable(frame->rdi, sizeof(uint32_t))) {
            return LINUX_SYSCALL_STATUS_BAD_ARGUMENT;
        }
        *return_value = 1U;
        return LINUX_SYSCALL_STATUS_OK;
    case 2U:
        *return_value = PAGING_LINUX_HEAP_BASE;
        return LINUX_SYSCALL_STATUS_OK;
    case 3U:
        if (map_heap() != LINUX_SYSCALL_STATUS_OK) {
            return LINUX_SYSCALL_STATUS_MAPPING;
        }
        *return_value = frame->rdi;
        return LINUX_SYSCALL_STATUS_OK;
    case 4U:
        if (paging_process_unmap_user_page(runtime.context.address_space,
                PAGING_PROCESS_MAPPING_LINUX_HEAP,
                PAGING_LINUX_HEAP_BASE) != PAGING_STATUS_OK) {
            return LINUX_SYSCALL_STATUS_BAD_ARGUMENT;
        }
        runtime.heap_mapped[0] = false;
        *return_value = PAGING_LINUX_HEAP_BASE;
        return LINUX_SYSCALL_STATUS_OK;
    case 5U:
        if (paging_process_map_user_page(runtime.context.address_space,
                PAGING_PROCESS_MAPPING_LINUX_ANON,
                PAGING_LINUX_ANON_ADDRESS, runtime.context.anonymous_frame,
                PAGING_WRITE) != PAGING_STATUS_OK) {
            return LINUX_SYSCALL_STATUS_BAD_ARGUMENT;
        }
        runtime.anonymous_mapped = true;
        *return_value = PAGING_LINUX_ANON_ADDRESS;
        return LINUX_SYSCALL_STATUS_OK;
    case 6U: {
        uint8_t output[LINUX_SYSCALL_STDOUT_BYTES];

        if (!copy_from_user(output, frame->rsi, sizeof(output))) {
            return LINUX_SYSCALL_STATUS_BAD_ARGUMENT;
        }
        for (size_t index = 0U; index < sizeof(output); ++index) {
            if (output[index] != expected_stdout[index]) {
                return LINUX_SYSCALL_STATUS_STDOUT;
            }
        }
        if (runtime.context.publish_stdout) {
            console_write_n((const char *)output, sizeof(output));
        }
        if (transition_stdout(STDOUT_SINK_WRITTEN) !=
                LINUX_SYSCALL_STATUS_OK) {
            return LINUX_SYSCALL_STATUS_BAD_STATE;
        }
        runtime.result.stdout_bytes = LINUX_SYSCALL_STDOUT_BYTES;
        runtime.result.stdout_valid = true;
        *return_value = LINUX_SYSCALL_STDOUT_BYTES;
        return LINUX_SYSCALL_STATUS_OK;
    }
    case 7U:
        if (paging_process_unmap_user_page(runtime.context.address_space,
                PAGING_PROCESS_MAPPING_LINUX_ANON,
                PAGING_LINUX_ANON_ADDRESS) != PAGING_STATUS_OK) {
            return LINUX_SYSCALL_STATUS_BAD_ARGUMENT;
        }
        runtime.anonymous_mapped = false;
        *return_value = 0U;
        return LINUX_SYSCALL_STATUS_OK;
    case 8U:
        if (!runtime.context.exit_observed(
                runtime.context.process_generation)) {
            return LINUX_SYSCALL_STATUS_EXIT;
        }
        runtime.result.exit_status = 0U;
        runtime.result.exit_zero = true;
        *return_value = 0U;
        return LINUX_SYSCALL_STATUS_OK;
    default:
        return LINUX_SYSCALL_STATUS_BAD_ORDER;
    }
}

uintptr_t linux_syscall_dispatch(struct linux_syscall_frame *frame)
{
    enum linux_syscall_status status = validate_entry(frame);
    uint64_t return_value = (uint64_t)(int64_t)-LINUX_ERRNO_EINVAL;
    size_t allowed_index;

    if (status != LINUX_SYSCALL_STATUS_OK) {
        return return_to_kernel(status);
    }
    if (transition_cpu(LINUX_SYSCALL_CPU_ENTERED) !=
            LINUX_SYSCALL_STATUS_OK) {
        return return_to_kernel(LINUX_SYSCALL_STATUS_BAD_STATE);
    }
    if (transition_provenance(PROVENANCE_ENTERED) !=
            LINUX_SYSCALL_STATUS_OK) {
        return return_to_kernel(LINUX_SYSCALL_STATUS_BAD_PROVENANCE);
    }
    ++runtime.request_ordinal;
    if (runtime.context.failure_before_ordinal == runtime.request_ordinal) {
        runtime.result.controlled_failure_observed = true;
        return return_to_kernel(LINUX_SYSCALL_STATUS_CONTROLLED_FAILURE);
    }
    allowed_index = allowlist_index(frame->rax);
    if (allowed_index == SIZE_MAX) {
        if (!enosys_result(frame->rax, &frame->rax)) {
            return return_to_kernel(LINUX_SYSCALL_STATUS_BAD_ARGUMENT);
        }
        if (transition_provenance(PROVENANCE_COMPLETED) !=
                LINUX_SYSCALL_STATUS_OK) {
            return return_to_kernel(LINUX_SYSCALL_STATUS_BAD_PROVENANCE);
        }
        if (transition_cpu(LINUX_SYSCALL_CPU_RETURNED) !=
                LINUX_SYSCALL_STATUS_OK) {
            return return_to_kernel(LINUX_SYSCALL_STATUS_BAD_STATE);
        }
        if (transition_provenance(PROVENANCE_CANDIDATE) !=
                LINUX_SYSCALL_STATUS_OK) {
            return return_to_kernel(LINUX_SYSCALL_STATUS_BAD_PROVENANCE);
        }
        return 0U;
    }
    if (runtime.call_index >= LINUX_SYSCALL_EXPECTED_CALLS ||
        frame->rax != expected_calls[runtime.call_index]) {
        return return_to_kernel(LINUX_SYSCALL_STATUS_BAD_ORDER);
    }
    status = execute_call(frame, &return_value);
    if (status != LINUX_SYSCALL_STATUS_OK) {
        return return_to_kernel(status);
    }
    if (!runtime.seen[allowed_index]) {
        runtime.seen[allowed_index] = true;
        ++runtime.result.distinct_syscalls;
    }
    ++runtime.call_index;
    runtime.result.syscall_count = runtime.call_index;
    runtime.result.real_syscall_instruction = true;
    runtime.result.process_authenticated = true;
    runtime.result.cr3_authenticated = true;
    if (transition_provenance(PROVENANCE_COMPLETED) !=
            LINUX_SYSCALL_STATUS_OK) {
        return return_to_kernel(LINUX_SYSCALL_STATUS_BAD_PROVENANCE);
    }
    frame->rax = return_value;
    if (runtime.context.failure_after_ordinal == runtime.request_ordinal) {
        runtime.result.controlled_failure_observed = true;
        return return_to_kernel(LINUX_SYSCALL_STATUS_CONTROLLED_FAILURE);
    }
    if (transition_cpu(LINUX_SYSCALL_CPU_RETURNED) !=
            LINUX_SYSCALL_STATUS_OK) {
        return return_to_kernel(LINUX_SYSCALL_STATUS_BAD_STATE);
    }
    if (transition_provenance(PROVENANCE_CANDIDATE) !=
            LINUX_SYSCALL_STATUS_OK) {
        return return_to_kernel(LINUX_SYSCALL_STATUS_BAD_PROVENANCE);
    }
    if (runtime.call_index != LINUX_SYSCALL_EXPECTED_CALLS) {
        return 0U;
    }
    if (paging_process_restore_kernel(runtime.context.address_space) !=
            PAGING_STATUS_OK) {
        runtime.result.status = LINUX_SYSCALL_STATUS_RESTORE;
    }
    return linux_process_resume_stack();
}

enum linux_syscall_status linux_syscall_disarm(void)
{
    enum linux_syscall_status result = LINUX_SYSCALL_STATUS_OK;

    if (!runtime.active) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    if (runtime.context.address_space == NULL ||
        runtime.context.address_space->state == PAGING_PROCESS_SPACE_ACTIVE ||
        linux_process_boundary_active() || cpu_interrupts_enabled() ||
        (cpu_read_cr3() & ~(PAGING_PAGE_SIZE - 1U)) !=
            paging_get_state().root_physical_address ||
        (runtime.state != LINUX_SYSCALL_CPU_ARMED &&
            runtime.state != LINUX_SYSCALL_CPU_RETURNED)) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    cpu_write_msr(IA32_FS_BASE, runtime.saved_fs_base);
    cpu_write_msr(IA32_FMASK, runtime.saved_fmask);
    cpu_write_msr(IA32_LSTAR, runtime.saved_lstar);
    cpu_write_msr(IA32_STAR, runtime.saved_star);
    cpu_write_msr(IA32_EFER, runtime.saved_efer);
    if (cpu_read_msr(IA32_FS_BASE) != runtime.saved_fs_base ||
        cpu_read_msr(IA32_FMASK) != runtime.saved_fmask ||
        cpu_read_msr(IA32_LSTAR) != runtime.saved_lstar ||
        cpu_read_msr(IA32_STAR) != runtime.saved_star ||
        cpu_read_msr(IA32_EFER) != runtime.saved_efer) {
        result = LINUX_SYSCALL_STATUS_MSR_CONTRACT;
    }
    if (runtime.stdout_state != STDOUT_SINK_RELEASED &&
        transition_stdout(STDOUT_SINK_RELEASED) !=
            LINUX_SYSCALL_STATUS_OK) {
        result = LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    if (runtime.provenance_state != PROVENANCE_RELEASED &&
        transition_provenance(PROVENANCE_RELEASED) !=
            LINUX_SYSCALL_STATUS_OK) {
        result = LINUX_SYSCALL_STATUS_BAD_PROVENANCE;
    }
    if (transition_cpu(LINUX_SYSCALL_CPU_DISARMED) !=
            LINUX_SYSCALL_STATUS_OK) {
        result = LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    runtime.result.cpu_disarmed = result == LINUX_SYSCALL_STATUS_OK;
    if (runtime.result.status == LINUX_SYSCALL_STATUS_OK) {
        runtime.result.status = result;
    }
    runtime.active = false;
    linux_syscall_kernel_stack = 0U;
    return result;
}

struct linux_syscall_result linux_syscall_get_result(void)
{
    return runtime.result;
}

bool linux_syscall_resources_released(void)
{
    return !runtime.active && linux_syscall_kernel_stack == 0U &&
        !linux_process_boundary_active();
}
