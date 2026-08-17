/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <seneri/acpi.h>
#include <seneri/acpi_util.h>
#include <seneri/apic.h>
#include <seneri/apic_timer.h>
#include <seneri/clock.h>
#include <seneri/console.h>
#include <seneri/cpu.h>
#include <seneri/interrupts.h>
#include <seneri/ioapic.h>
#include <seneri/pic.h>
#include <seneri/pit.h>
#include <seneri/pm_timer.h>
#include <seneri/test.h>
#include <seneri/timer.h>
#include <seneri/tsc.h>

#define QEMU_EXIT_PORT UINT16_C(0x00F4)
#define QEMU_FAILURE_VALUE UINT8_C(0x7F)
#define PAGE_FAULT_TEST_ADDRESS UINT64_C(0x0000000100000000)
#define PIT_TEST_FREQUENCY UINT32_C(100)
#define PIT_TEST_TICKS UINT64_C(8)
#define APIC_TIMER_TEST_FREQUENCY UINT32_C(100)
#define APIC_TIMER_TEST_TICKS UINT64_C(20)
#define TSC_MONOTONIC_READS 64U

/*
 * Ten milliseconds of the ACPI timer, which counts at 3.579545 MHz, and the
 * 200 ms interval the local APIC timer defines by counting twenty of its own
 * ticks at 100 Hz. Two hundred milliseconds is 4.3% of the narrowest counter's
 * period, so the measurement stays far inside a single wrap.
 */
#define PM_TIMER_TEST_TICKS UINT32_C(35795)
#define PM_TIMER_TEST_FREQUENCY UINT32_C(100)
#define PM_TIMER_TEST_APIC_TICKS UINT64_C(20)

/*
 * Three deadlines, 20 ms apart. Far enough apart that the fixed cost of
 * reprogramming between them cannot reorder them, and short enough that the
 * whole scenario stays well inside its QEMU timeout.
 */
#define TIMERS_TEST_COUNT 3U
#define TIMERS_TEST_STEP_NS UINT64_C(20000000)

volatile uint8_t kernel_test_double_fault_armed;
static enum kernel_test_scenario active_scenario;

static size_t literal_length(const char *text)
{
    size_t length = 0;

    while (text[length] != '\0') {
        ++length;
    }

    return length;
}

static bool token_equals(const char *token, size_t token_length, const char *literal)
{
    const size_t expected_length = literal_length(literal);

    if (token_length != expected_length) {
        return false;
    }

    for (size_t index = 0; index < token_length; ++index) {
        if (token[index] != literal[index]) {
            return false;
        }
    }

    return true;
}

static bool token_has_prefix(
    const char *token,
    size_t token_length,
    const char *prefix,
    size_t *value_offset
)
{
    const size_t prefix_length = literal_length(prefix);

    if (token_length < prefix_length) {
        return false;
    }

    for (size_t index = 0; index < prefix_length; ++index) {
        if (token[index] != prefix[index]) {
            return false;
        }
    }

    *value_offset = prefix_length;
    return true;
}

static enum kernel_test_scenario scenario_from_value(
    const char *value,
    size_t length
)
{
    if (token_equals(value, length, "normal")) {
        return KERNEL_TEST_NORMAL;
    }

    if (token_equals(value, length, "breakpoint")) {
        return KERNEL_TEST_BREAKPOINT;
    }

    if (token_equals(value, length, "invalid-opcode")) {
        return KERNEL_TEST_INVALID_OPCODE;
    }

    if (token_equals(value, length, "page-fault")) {
        return KERNEL_TEST_PAGE_FAULT;
    }

    if (token_equals(value, length, "ist")) {
        return KERNEL_TEST_IST;
    }

    if (token_equals(value, length, "pit")) {
        return KERNEL_TEST_PIT;
    }

    if (token_equals(value, length, "unexpected")) {
        return KERNEL_TEST_UNEXPECTED;
    }

    if (token_equals(value, length, "double-fault")) {
        return KERNEL_TEST_DOUBLE_FAULT;
    }

    if (token_equals(value, length, "apic")) {
        return KERNEL_TEST_APIC;
    }

    if (token_equals(value, length, "ioapic")) {
        return KERNEL_TEST_IOAPIC;
    }

    if (token_equals(value, length, "retired")) {
        return KERNEL_TEST_RETIRED;
    }

    if (token_equals(value, length, "apic-timer")) {
        return KERNEL_TEST_APIC_TIMER;
    }

    if (token_equals(value, length, "tsc")) {
        return KERNEL_TEST_TSC;
    }

    if (token_equals(value, length, "pm-timer")) {
        return KERNEL_TEST_PM_TIMER;
    }

    if (token_equals(value, length, "pit-retired")) {
        return KERNEL_TEST_PIT_RETIRED;
    }

    if (token_equals(value, length, "timers")) {
        return KERNEL_TEST_TIMERS;
    }

    return KERNEL_TEST_INVALID;
}

static uint8_t scenario_exit_value(enum kernel_test_scenario scenario)
{
    switch (scenario) {
    case KERNEL_TEST_NORMAL:
        return UINT8_C(0x10);
    case KERNEL_TEST_BREAKPOINT:
        return UINT8_C(0x11);
    case KERNEL_TEST_INVALID_OPCODE:
        return UINT8_C(0x12);
    case KERNEL_TEST_PAGE_FAULT:
        return UINT8_C(0x13);
    case KERNEL_TEST_IST:
        return UINT8_C(0x14);
    case KERNEL_TEST_PIT:
        return UINT8_C(0x15);
    case KERNEL_TEST_UNEXPECTED:
        return UINT8_C(0x16);
    case KERNEL_TEST_DOUBLE_FAULT:
        return UINT8_C(0x17);
    case KERNEL_TEST_APIC:
        return UINT8_C(0x18);
    case KERNEL_TEST_IOAPIC:
        return UINT8_C(0x19);
    case KERNEL_TEST_RETIRED:
        return UINT8_C(0x1A);
    case KERNEL_TEST_APIC_TIMER:
        return UINT8_C(0x1B);
    case KERNEL_TEST_TSC:
        return UINT8_C(0x1C);
    case KERNEL_TEST_PM_TIMER:
        return UINT8_C(0x1D);
    case KERNEL_TEST_PIT_RETIRED:
        return UINT8_C(0x1E);
    case KERNEL_TEST_TIMERS:
        return UINT8_C(0x1F);
    default:
        return QEMU_FAILURE_VALUE;
    }
}

static void test_marker(const char *kind, enum kernel_test_scenario scenario)
{
    console_write("ST ");
    console_write(kind);
    console_putc(' ');
    console_write(kernel_test_scenario_name(scenario));
    console_putc('\n');
}

static _Noreturn void kernel_test_pass(void)
{
    const uint8_t exit_value = scenario_exit_value(active_scenario);

    test_marker("PASS", active_scenario);
    cpu_out32(QEMU_EXIT_PORT, exit_value);
    console_halt();
}

enum kernel_test_scenario kernel_test_select(const struct boot_context *context)
{
    static const char prefix[] = "seneri.test=";
    enum kernel_test_scenario selected = KERNEL_TEST_NONE;
    size_t offset = 0;

    kernel_test_double_fault_armed = 0U;
    active_scenario = KERNEL_TEST_NONE;

    if (context == NULL || context->command_line == NULL) {
        return KERNEL_TEST_NONE;
    }

    while (offset < context->command_line_length) {
        size_t token_start;
        size_t token_length;
        size_t value_offset;

        while (offset < context->command_line_length &&
               context->command_line[offset] == ' ') {
            ++offset;
        }

        token_start = offset;

        while (offset < context->command_line_length &&
               context->command_line[offset] != ' ') {
            ++offset;
        }

        token_length = offset - token_start;

        if (token_length == 0U || !token_has_prefix(
                context->command_line + token_start,
                token_length,
                prefix,
                &value_offset
            )) {
            continue;
        }

        if (selected != KERNEL_TEST_NONE) {
            return KERNEL_TEST_INVALID;
        }

        selected = scenario_from_value(
            context->command_line + token_start + value_offset,
            token_length - value_offset
        );

        if (selected == KERNEL_TEST_INVALID) {
            return selected;
        }
    }

    active_scenario = selected;
    return selected;
}

/*
 * Enabling the local APIC takes the 8259 pair off the processor's direct
 * interrupt path. This scenario proves the replacement path: the APIC is online
 * and agrees with firmware, and the PIT still delivers through LINT0.
 */
static void apic_scenario(void)
{
    const struct apic_state apic = apic_get_state();
    enum pit_status pit_status;

    if (!apic_is_online() || !apic.online) {
        kernel_test_fail("local APIC is not online");
    }

    if (apic.base_address == 0U || apic.max_lvt_entry < 4U) {
        kernel_test_fail("local APIC reported an unusable register window");
    }

    if (!apic.legacy_interrupts_routed) {
        kernel_test_fail("local APIC did not route legacy interrupts");
    }

    pit_status = pit_start(PIT_TEST_FREQUENCY, PIT_ROUTE_LEGACY_PIC);

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    pit_status = pit_wait_for_ticks(PIT_TEST_TICKS);

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    pit_status = pit_stop();

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    if (pit_ticks() < PIT_TEST_TICKS) {
        kernel_test_fail("PIT stopped delivering once the local APIC was on");
    }

    if (apic_spurious_count() != 0U) {
        kernel_test_fail("local APIC raised a spurious interrupt");
    }
}

/*
 * Prove the timer arrives through the I/O APIC rather than the 8259 pair: the
 * legacy line stays masked, the redirection entry carries the ACPI override,
 * and the interrupt is acknowledged at the local APIC.
 */
static void ioapic_scenario(void)
{
    const struct ioapic_state ioapic = ioapic_get_state();
    enum pit_status pit_status;

    if (!ioapic_is_initialized() || ioapic.count == 0U) {
        kernel_test_fail("I/O APIC is not initialized");
    }

    if (ioapic.units[0].entry_count < 16U) {
        kernel_test_fail("I/O APIC cannot redirect the ISA interrupts");
    }

    if (pic_mask_snapshot() != UINT16_C(0xFFFF)) {
        kernel_test_fail("a legacy PIC line was left unmasked");
    }

    pit_status = pit_start(PIT_TEST_FREQUENCY, PIT_ROUTE_IO_APIC);

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    if (pit_active_route() != PIT_ROUTE_IO_APIC) {
        kernel_test_fail("timer did not take the I/O APIC route");
    }

    if (pic_mask_snapshot() != UINT16_C(0xFFFF)) {
        kernel_test_fail("I/O APIC routing unmasked a legacy PIC line");
    }

    pit_status = pit_wait_for_ticks(PIT_TEST_TICKS);

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    pit_status = pit_stop();

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    if (pit_ticks() < PIT_TEST_TICKS) {
        kernel_test_fail("I/O APIC delivered too few timer interrupts");
    }

    if (apic_spurious_count() != 0U) {
        kernel_test_fail("local APIC raised a spurious interrupt");
    }
}

/*
 * Retire the 8259 pair and prove the machine keeps its timer. This is the
 * scenario that would catch a retirement which silently took interrupt
 * delivery with it.
 */
static void retired_scenario(void)
{
    enum pit_status pit_status;
    enum pic_status pic_status;

    if (!pic_is_initialized() || pic_is_retired()) {
        kernel_test_fail("legacy PIC was not in its expected initial state");
    }

    pic_status = pic_retire();

    if (pic_status != PIC_STATUS_OK) {
        kernel_test_fail(pic_status_string(pic_status));
    }

    if (apic_retire_legacy_routing() != APIC_STATUS_OK) {
        kernel_test_fail("local APIC kept carrying legacy interrupts");
    }

    if (!pic_is_retired() || pic_mask_snapshot() != UINT16_C(0xFFFF)) {
        kernel_test_fail("legacy PIC is not fully masked after retirement");
    }

    if (pic_set_mask(0U, false) != PIC_STATUS_RETIRED ||
        pic_retire() != PIC_STATUS_RETIRED) {
        kernel_test_fail("retired PIC accepted a further mutation");
    }

    if (apic_get_state().legacy_interrupts_routed) {
        kernel_test_fail("local APIC still reports legacy routing");
    }

    pit_status = pit_start(PIT_TEST_FREQUENCY, PIT_ROUTE_IO_APIC);

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    pit_status = pit_wait_for_ticks(PIT_TEST_TICKS);

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    pit_status = pit_stop();

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    if (pit_ticks() < PIT_TEST_TICKS) {
        kernel_test_fail("timer stopped once the legacy PIC was retired");
    }

    if (apic_spurious_count() != 0U) {
        kernel_test_fail("local APIC raised a spurious interrupt");
    }
}

/*
 * Calibrate and run the local APIC timer, then check the rate it measured is
 * consistent with the reference it measured against. A timer that ticks but
 * counts at the wrong rate is the failure this scenario exists to find.
 */
static void apic_timer_scenario(void)
{
    enum apic_timer_status status;
    uint64_t elapsed_ticks;
    uint64_t measured_ns;
    uint64_t expected_ns;
    uint32_t start;
    uint32_t span = 0U;

    if (!apic_is_online()) {
        kernel_test_fail("local APIC is not online");
    }

    if (apic_timer_is_calibrated() || apic_timer_is_running()) {
        kernel_test_fail("local APIC timer was already in use");
    }

    if (apic_timer_start(APIC_TIMER_TEST_FREQUENCY) !=
        APIC_TIMER_STATUS_NOT_CALIBRATED) {
        kernel_test_fail("uncalibrated local APIC timer agreed to run");
    }

    status = apic_timer_calibrate();

    if (status != APIC_TIMER_STATUS_OK) {
        kernel_test_fail(apic_timer_status_string(status));
    }

    if (apic_timer_counts_per_second() == 0U) {
        kernel_test_fail("local APIC timer calibrated to a zero rate");
    }

    if (apic_timer_calibrate() != APIC_TIMER_STATUS_ALREADY_CALIBRATED) {
        kernel_test_fail("local APIC timer accepted a second calibration");
    }

    status = apic_timer_start(APIC_TIMER_TEST_FREQUENCY);

    if (status != APIC_TIMER_STATUS_OK) {
        kernel_test_fail(apic_timer_status_string(status));
    }

    if (apic_timer_start(APIC_TIMER_TEST_FREQUENCY) !=
        APIC_TIMER_STATUS_ALREADY_RUNNING) {
        kernel_test_fail("local APIC timer started twice");
    }

    /*
     * Let the timer count out a known number of its own ticks and measure how
     * long that took on the ACPI timer it was calibrated from. Checking the
     * duration rather than a tick count catches the failure a tick count cannot:
     * a timer whose rate is wrong still delivers every tick it is asked for, it
     * just takes the wrong amount of time doing it.
     */
    start = pm_timer_read();

    if (apic_timer_wait_for_ticks(APIC_TIMER_TEST_TICKS) !=
        APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer stopped delivering");
    }

    elapsed_ticks = apic_timer_ticks();
    status = apic_timer_stop();

    if (status != APIC_TIMER_STATUS_OK) {
        kernel_test_fail(apic_timer_status_string(status));
    }

    if (pm_timer_span(start, pm_timer_read(), &span) != PM_TIMER_STATUS_OK) {
        kernel_test_fail("reference clock reported no duration");
    }

    if (elapsed_ticks < APIC_TIMER_TEST_TICKS) {
        kernel_test_fail("local APIC timer delivered too few interrupts");
    }

    measured_ns = pm_timer_ticks_to_nanoseconds(span);
    expected_ns = APIC_TIMER_TEST_TICKS * UINT64_C(1000000000) /
        APIC_TIMER_TEST_FREQUENCY;

    if (!pm_timer_durations_agree(
            measured_ns,
            expected_ns,
            PM_TIMER_TOLERANCE_QUARTER
        )) {
        kernel_test_fail("local APIC timer rate disagrees with its reference");
    }

    if (apic_spurious_count() != 0U) {
        kernel_test_fail("local APIC raised a spurious interrupt");
    }
}

/*
 * Establish the time-stamp counter as a second reference and check it against
 * the first. Two clocks calibrated from the same ruler must agree about the
 * same interval; a clock that only agrees with itself proves nothing.
 */
static void tsc_scenario(void)
{
    struct tsc_state tsc;
    uint64_t previous;
    uint64_t start;
    uint64_t measured_ns;
    uint64_t expected_ns;
    enum tsc_status status;

    if (tsc_is_calibrated()) {
        kernel_test_fail("TSC was already calibrated");
    }

    if (tsc_span_nanoseconds(0U, UINT64_C(1000)) != 0U) {
        kernel_test_fail("uncalibrated TSC reported a duration");
    }

    status = tsc_calibrate();

    if (status != TSC_STATUS_OK) {
        kernel_test_fail(tsc_status_string(status));
    }

    tsc = tsc_get_state();

    if (!tsc.present || tsc.frequency_hz == 0U) {
        kernel_test_fail("TSC calibrated to an unusable rate");
    }

    if (tsc_calibrate() != TSC_STATUS_ALREADY_CALIBRATED) {
        kernel_test_fail("TSC accepted a second calibration");
    }

    /* A counter that steps backwards cannot order anything. */
    previous = tsc_read();

    for (size_t index = 0; index < TSC_MONOTONIC_READS; ++index) {
        const uint64_t current = tsc_read();

        if (current < previous) {
            kernel_test_fail("TSC ran backwards between reads");
        }

        previous = current;
    }

    if (apic_timer_calibrate() != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("second clock would not calibrate");
    }

    if (apic_timer_start(APIC_TIMER_TEST_FREQUENCY) != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("second clock would not start");
    }

    start = tsc_read();

    if (apic_timer_wait_for_ticks(APIC_TIMER_TEST_TICKS) !=
        APIC_TIMER_STATUS_OK) {
        kernel_test_fail("second clock stopped delivering");
    }

    measured_ns = tsc_span_nanoseconds(start, tsc_read());

    if (apic_timer_stop() != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("second clock would not stop");
    }

    expected_ns = APIC_TIMER_TEST_TICKS * UINT64_C(1000000000) /
        APIC_TIMER_TEST_FREQUENCY;

    if (measured_ns < expected_ns / 2U || measured_ns > expected_ns * 2U) {
        kernel_test_fail("TSC and local APIC timer disagree about an interval");
    }
}

/*
 * Establish the ACPI power management timer as an independent reference, then
 * check the two calibrated clocks against it.
 *
 * The local APIC timer and the TSC were both measured against the PIT, so they
 * agree with each other even if that shared measurement was wrong. This timer's
 * rate is fixed by the ACPI specification and is measured against nothing, so
 * one interval described by all three is the first evidence that the PIT
 * measurement itself was right. Retiring the PIT is the increment after this
 * one, and only on the strength of this agreement.
 */
static void pm_timer_scenario(void)
{
    struct pm_timer_state pm;
    struct acpi_fadt probe;
    uint64_t waited_ticks = 0U;
    uint64_t tsc_start;
    uint64_t measured_ns;
    uint64_t reference_ns;
    uint64_t expected_ns;
    uint32_t start;
    uint32_t end;
    uint32_t span = 0U;

    if (!pm_timer_is_present()) {
        kernel_test_fail("ACPI PM timer was not discovered during boot");
    }

    pm = pm_timer_get_state();

    if (pm.port == 0U ||
        (pm.counter_bits != ACPI_PM_TIMER_BASE_BITS &&
         pm.counter_bits != ACPI_PM_TIMER_EXTENDED_BITS)) {
        kernel_test_fail("ACPI PM timer reported an unusable description");
    }

    /*
     * The timer is discovered once. A second description is refused before any
     * of its fields are read, so a zeroed one is enough to prove the refusal.
     */
    acpi_bytes_zero(&probe, sizeof(probe));

    if (pm_timer_initialize(&probe) != PM_TIMER_STATUS_ALREADY_INITIALIZED) {
        kernel_test_fail("ACPI PM timer accepted a second description");
    }

    /* The counter has to advance on its own before it can time anything. */
    if (pm_timer_wait(PM_TIMER_TEST_TICKS, &waited_ticks) !=
        PM_TIMER_STATUS_OK) {
        kernel_test_fail("ACPI PM timer did not advance within its bound");
    }

    if (waited_ticks < PM_TIMER_TEST_TICKS) {
        kernel_test_fail("ACPI PM timer wait returned early");
    }

    if (tsc_calibrate() != TSC_STATUS_OK) {
        kernel_test_fail("time-stamp counter would not calibrate");
    }

    if (apic_timer_calibrate() != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer would not calibrate");
    }

    if (apic_timer_start(PM_TIMER_TEST_FREQUENCY) != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer would not start");
    }

    /*
     * One interval, three opinions. The APIC timer defines it by counting its
     * own ticks; the PM timer and the TSC each measure it without being told.
     */
    start = pm_timer_read();
    tsc_start = tsc_read();

    if (apic_timer_wait_for_ticks(PM_TIMER_TEST_APIC_TICKS) !=
        APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer stopped delivering");
    }

    end = pm_timer_read();
    reference_ns = tsc_span_nanoseconds(tsc_start, tsc_read());

    if (apic_timer_stop() != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer would not stop");
    }

    if (pm_timer_span(start, end, &span) != PM_TIMER_STATUS_OK) {
        kernel_test_fail("ACPI PM timer span is not a duration");
    }

    measured_ns = pm_timer_ticks_to_nanoseconds(span);
    expected_ns = PM_TIMER_TEST_APIC_TICKS * UINT64_C(1000000000) /
        PM_TIMER_TEST_FREQUENCY;

    console_write("ST INFO pm-timer: PM ");
    console_write_u64(measured_ns);
    console_write(" ns, APIC timer ");
    console_write_u64(expected_ns);
    console_write(" ns, TSC ");
    console_write_u64(reference_ns);
    console_write(" ns\n");

    /*
     * The local APIC timer is held to the tight bound: it and the PIT it was
     * calibrated against are driven from the same source under emulation, so
     * this comparison is the one that must catch a rate wrong by a factor.
     */
    if (!pm_timer_durations_agree(
            measured_ns,
            expected_ns,
            PM_TIMER_TOLERANCE_QUARTER
        )) {
        kernel_test_fail("PM timer and local APIC timer disagree on interval");
    }

    if (!pm_timer_durations_agree(
            measured_ns,
            reference_ns,
            PM_TIMER_TOLERANCE_HALF
        )) {
        kernel_test_fail("PM timer and TSC disagree about an interval");
    }

    if (apic_spurious_count() != 0U) {
        kernel_test_fail("local APIC raised a spurious interrupt");
    }
}

/*
 * Retire the 8254 and prove the machine keeps its clocks.
 *
 * This is the mirror of the `retired` scenario, which proved the machine keeps
 * its timer after the 8259 pair is latched shut. Here the timer itself goes: the
 * PIT is proved working, retired, and then refuses every further mutation, and
 * both derived clocks are calibrated and cross-checked with it dead. Calibration
 * used to spin the PIT, so a retirement that broke that path would show up here
 * as a clock that will not calibrate at all rather than as one running slow.
 */
static void pit_retired_scenario(void)
{
    struct pm_timer_state pm;
    uint64_t tsc_start;
    uint64_t measured_ns;
    uint64_t reference_ns;
    uint64_t expected_ns;
    uint32_t start;
    uint32_t span = 0U;

    if (!pm_timer_is_present()) {
        kernel_test_fail("ACPI PM timer was not discovered during boot");
    }

    pm = pm_timer_get_state();

    if (pm.port == 0U) {
        kernel_test_fail("ACPI PM timer reported an unusable description");
    }

    /* The PIT still works at this point, and is proved so before it goes. */
    if (pit_is_retired()) {
        kernel_test_fail("PIT was already retired");
    }

    if (pit_start(PIT_TEST_FREQUENCY, PIT_ROUTE_IO_APIC) != PIT_STATUS_OK) {
        kernel_test_fail("PIT would not start before its retirement");
    }

    if (pit_wait_for_ticks(PIT_TEST_TICKS) != PIT_STATUS_OK) {
        kernel_test_fail("PIT would not deliver before its retirement");
    }

    if (pit_retire() != PIT_STATUS_OK) {
        kernel_test_fail("PIT refused to retire");
    }

    /* A retired PIT is latched: running, restarting and re-retiring all fail. */
    if (!pit_is_retired() || pit_is_running()) {
        kernel_test_fail("PIT is not fully retired");
    }

    if (pit_start(PIT_TEST_FREQUENCY, PIT_ROUTE_IO_APIC) !=
            PIT_STATUS_RETIRED ||
        pit_start(PIT_TEST_FREQUENCY, PIT_ROUTE_LEGACY_PIC) !=
            PIT_STATUS_RETIRED ||
        pit_retire() != PIT_STATUS_RETIRED) {
        kernel_test_fail("retired PIT accepted a further mutation");
    }

    /* Both derived clocks must calibrate with no PIT to lean on. */
    if (tsc_calibrate() != TSC_STATUS_OK) {
        kernel_test_fail("TSC would not calibrate without the PIT");
    }

    if (apic_timer_calibrate() != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer would not calibrate without the PIT");
    }

    if (apic_timer_counts_per_second() == 0U || tsc_frequency() == 0U) {
        kernel_test_fail("a clock calibrated to an unusable rate");
    }

    if (apic_timer_start(PM_TIMER_TEST_FREQUENCY) != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer would not start");
    }

    start = pm_timer_read();
    tsc_start = tsc_read();

    if (apic_timer_wait_for_ticks(PM_TIMER_TEST_APIC_TICKS) !=
        APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer stopped delivering");
    }

    reference_ns = tsc_span_nanoseconds(tsc_start, tsc_read());

    if (apic_timer_stop() != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer would not stop");
    }

    if (pm_timer_span(start, pm_timer_read(), &span) != PM_TIMER_STATUS_OK) {
        kernel_test_fail("ACPI PM timer span is not a duration");
    }

    measured_ns = pm_timer_ticks_to_nanoseconds(span);
    expected_ns = PM_TIMER_TEST_APIC_TICKS * UINT64_C(1000000000) /
        PM_TIMER_TEST_FREQUENCY;

    console_write("ST INFO pit-retired: PM ");
    console_write_u64(measured_ns);
    console_write(" ns, APIC timer ");
    console_write_u64(expected_ns);
    console_write(" ns, TSC ");
    console_write_u64(reference_ns);
    console_write(" ns\n");

    if (!pm_timer_durations_agree(
            measured_ns,
            expected_ns,
            PM_TIMER_TOLERANCE_QUARTER
        )) {
        kernel_test_fail("clocks disagree on an interval without the PIT");
    }

    if (!pm_timer_durations_agree(
            measured_ns,
            reference_ns,
            PM_TIMER_TOLERANCE_HALF
        )) {
        kernel_test_fail("PM timer and TSC disagree without the PIT");
    }

    if (apic_spurious_count() != 0U) {
        kernel_test_fail("local APIC raised a spurious interrupt");
    }
}

/*
 * Written by timer callbacks inside the timer interrupt and read by the scenario
 * outside it, so the compiler must not keep either in a register across the halt
 * inside a sleep.
 */
static volatile uint32_t timers_fired[TIMERS_TEST_COUNT];
static volatile size_t timers_fired_count;

static void timers_record(uint64_t deadline_ns, void *context)
{
    (void)deadline_ns;

    if (timers_fired_count < TIMERS_TEST_COUNT) {
        timers_fired[timers_fired_count] = *(const uint32_t *)context;
        ++timers_fired_count;
    }
}

/*
 * Establish the monotonic clock and deadline timers, and prove the two things
 * that make them usable: the clock never steps backwards, and a deadline arrives
 * after the instant it named rather than before it.
 *
 * A sleep that returns early is the failure worth hunting. It would not look
 * like a failure - the call returns, the callback ran - and every wait built on
 * it would be silently short. So the scenario checks the elapsed time against
 * the clock rather than trusting that the callback fired.
 */
static void timers_scenario(void)
{
    static const uint32_t labels[TIMERS_TEST_COUNT] = {1U, 2U, 3U};
    uint64_t identifiers[TIMERS_TEST_COUNT] = {0U, 0U, 0U};
    uint64_t previous;
    uint64_t start;
    uint64_t slept_ns;
    uint64_t spare = 0U;
    uint64_t now;

    /* Before the clock has an origin it reports nothing rather than garbage. */
    if (clock_is_started() || clock_monotonic_ns() != 0U) {
        kernel_test_fail("monotonic clock was already started");
    }

    if (clock_start() != CLOCK_STATUS_NO_SOURCE) {
        kernel_test_fail("clock started without a calibrated counter");
    }

    if (tsc_calibrate() != TSC_STATUS_OK) {
        kernel_test_fail("TSC would not calibrate");
    }

    if (apic_timer_calibrate() != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer would not calibrate");
    }

    if (clock_start() != CLOCK_STATUS_OK) {
        kernel_test_fail("monotonic clock would not start");
    }

    if (clock_start() != CLOCK_STATUS_ALREADY_STARTED) {
        kernel_test_fail("monotonic clock started twice");
    }

    /* A clock that steps backwards cannot order anything. */
    previous = clock_monotonic_ns();

    for (size_t index = 0; index < TSC_MONOTONIC_READS; ++index) {
        now = clock_monotonic_ns();

        if (now < previous) {
            kernel_test_fail("monotonic clock stepped backwards");
        }

        previous = now;
    }

    if (clock_get_state().backward_steps != 0U) {
        kernel_test_fail("monotonic clock had to repair a reading");
    }

    /* Deadlines need the clock, and refuse to run without it. */
    if (timer_arm(previous + TIMERS_TEST_STEP_NS, timers_record, NULL, &spare) !=
        TIMER_STATUS_NOT_STARTED) {
        kernel_test_fail("deadline armed before the timers were started");
    }

    if (timer_start() != TIMER_STATUS_OK) {
        kernel_test_fail("deadline timers would not start");
    }

    if (timer_start() != TIMER_STATUS_ALREADY_STARTED) {
        kernel_test_fail("deadline timers started twice");
    }

    /* A deadline already gone, and one too near to program, are both refused. */
    now = clock_monotonic_ns();

    if (timer_arm(0U, timers_record, NULL, &spare) !=
            TIMER_STATUS_BAD_INTERVAL ||
        spare != 0U ||
        timer_arm(now + 1U, timers_record, NULL, &spare) !=
            TIMER_STATUS_BAD_INTERVAL) {
        kernel_test_fail("a deadline in the past was accepted");
    }

    if (timer_cancel(0U) != TIMER_STATUS_UNKNOWN_TIMER ||
        timer_cancel(UINT64_MAX) != TIMER_STATUS_UNKNOWN_TIMER) {
        kernel_test_fail("cancelling an unknown deadline was accepted");
    }

    /* Three deadlines, armed out of order, must fire in time order. */
    timers_fired_count = 0U;
    start = clock_monotonic_ns();

    for (size_t index = 0; index < TIMERS_TEST_COUNT; ++index) {
        const size_t reversed = TIMERS_TEST_COUNT - 1U - index;

        if (timer_arm(
                start + TIMERS_TEST_STEP_NS * (uint64_t)(reversed + 1U),
                timers_record,
                (void *)&labels[reversed],
                &identifiers[reversed]
            ) != TIMER_STATUS_OK ||
            identifiers[reversed] == 0U) {
            kernel_test_fail("a deadline would not arm");
        }
    }

    if (timer_pending_count() != TIMERS_TEST_COUNT) {
        kernel_test_fail("deadline timer table lost an entry");
    }

    /* Sleeping past all three collects them; the sleep is itself a deadline. */
    slept_ns = clock_monotonic_ns();

    if (timer_sleep_ns(TIMERS_TEST_STEP_NS * (TIMERS_TEST_COUNT + 1U)) !=
        TIMER_STATUS_OK) {
        kernel_test_fail("sleep did not complete");
    }

    slept_ns = clock_monotonic_ns() - slept_ns;

    if (timers_fired_count != TIMERS_TEST_COUNT) {
        kernel_test_fail("not every deadline fired");
    }

    for (size_t index = 0; index < TIMERS_TEST_COUNT; ++index) {
        if (timers_fired[index] != labels[index]) {
            kernel_test_fail("deadlines fired out of order");
        }
    }

    if (slept_ns < TIMERS_TEST_STEP_NS * (TIMERS_TEST_COUNT + 1U)) {
        kernel_test_fail("sleep returned before its deadline");
    }

    if (timer_pending_count() != 0U || timer_expiry_count() == 0U) {
        kernel_test_fail("deadline timer table did not settle");
    }

    /* A cancelled deadline must not fire, and its identifier must go stale. */
    timers_fired_count = 0U;
    now = clock_monotonic_ns();

    if (timer_arm(
            now + TIMERS_TEST_STEP_NS,
            timers_record,
            (void *)&labels[0],
            &spare
        ) != TIMER_STATUS_OK) {
        kernel_test_fail("a deadline would not arm for cancellation");
    }

    if (timer_cancel(spare) != TIMER_STATUS_OK ||
        timer_cancel(spare) != TIMER_STATUS_UNKNOWN_TIMER ||
        timer_pending_count() != 0U) {
        kernel_test_fail("cancelling a deadline did not release it");
    }

    if (timer_sleep_ns(TIMERS_TEST_STEP_NS * 2U) != TIMER_STATUS_OK) {
        kernel_test_fail("sleep after a cancellation did not complete");
    }

    if (timers_fired_count != 0U) {
        kernel_test_fail("a cancelled deadline fired anyway");
    }

    if (timer_stop() != TIMER_STATUS_OK ||
        timer_is_started() ||
        timer_stop() != TIMER_STATUS_NOT_STARTED) {
        kernel_test_fail("deadline timers would not stop");
    }

    if (apic_spurious_count() != 0U) {
        kernel_test_fail("local APIC raised a spurious interrupt");
    }
}

void kernel_test_run(enum kernel_test_scenario scenario)
{
    enum pit_status pit_status;

    if (scenario == KERNEL_TEST_NONE) {
        return;
    }

    active_scenario = scenario;
    test_marker("BEGIN", scenario);

    if (!interrupt_frame_layout_self_test()) {
        kernel_test_fail("interrupt frame or descriptor validation failed");
    }

    switch (scenario) {
    case KERNEL_TEST_NORMAL:
        return;
    case KERNEL_TEST_BREAKPOINT:
        if (!interrupt_breakpoint_self_test()) {
            kernel_test_fail("breakpoint register restoration failed");
        }
        kernel_test_pass();
    case KERNEL_TEST_INVALID_OPCODE:
        interrupt_trigger_invalid_opcode();
    case KERNEL_TEST_PAGE_FAULT:
        interrupt_trigger_page_fault();
    case KERNEL_TEST_IST:
        if (!interrupt_ist_self_test()) {
            kernel_test_fail("IST routing proof failed");
        }
        kernel_test_pass();
    case KERNEL_TEST_PIT:
        pit_status = pit_start(PIT_TEST_FREQUENCY, PIT_ROUTE_LEGACY_PIC);

        if (pit_status != PIT_STATUS_OK) {
            kernel_test_fail(pit_status_string(pit_status));
        }

        pit_status = pit_wait_for_ticks(PIT_TEST_TICKS);

        if (pit_status != PIT_STATUS_OK) {
            kernel_test_fail(pit_status_string(pit_status));
        }

        if (pit_ticks() < PIT_TEST_TICKS) {
            kernel_test_fail("PIT delivered too few ticks");
        }

        pit_status = pit_stop();

        if (pit_status != PIT_STATUS_OK) {
            kernel_test_fail(pit_status_string(pit_status));
        }

        kernel_test_pass();
    case KERNEL_TEST_UNEXPECTED:
        interrupt_trigger_unexpected();
    case KERNEL_TEST_APIC:
        apic_scenario();
        kernel_test_pass();
    case KERNEL_TEST_IOAPIC:
        ioapic_scenario();
        kernel_test_pass();
    case KERNEL_TEST_RETIRED:
        retired_scenario();
        kernel_test_pass();
    case KERNEL_TEST_APIC_TIMER:
        apic_timer_scenario();
        kernel_test_pass();
    case KERNEL_TEST_TSC:
        tsc_scenario();
        kernel_test_pass();
    case KERNEL_TEST_PM_TIMER:
        pm_timer_scenario();
        kernel_test_pass();
    case KERNEL_TEST_PIT_RETIRED:
        pit_retired_scenario();
        kernel_test_pass();
    case KERNEL_TEST_TIMERS:
        timers_scenario();
        kernel_test_pass();
    case KERNEL_TEST_DOUBLE_FAULT:
        kernel_test_double_fault_armed = 1U;
        interrupt_test_set_gate_present(14U, false);
        interrupt_trigger_page_fault();
    case KERNEL_TEST_INVALID:
        kernel_test_fail("invalid or duplicate seneri.test argument");
    case KERNEL_TEST_NONE:
    default:
        kernel_test_fail("unreachable test scenario");
    }
}

_Noreturn void kernel_test_complete_normal(void)
{
    if (active_scenario != KERNEL_TEST_NORMAL) {
        kernel_test_fail("normal completion used outside the normal scenario");
    }

    kernel_test_pass();
}

bool kernel_test_handle_fatal_interrupt(const struct interrupt_frame *frame)
{
    bool matches = false;

    if (frame == NULL) {
        return false;
    }

    switch (active_scenario) {
    case KERNEL_TEST_INVALID_OPCODE:
        matches = frame->vector == 6U &&
            frame->error_code == 0U &&
            frame->rip == (uintptr_t)(const void *)interrupt_invalid_opcode_site;
        break;
    case KERNEL_TEST_PAGE_FAULT:
        matches = frame->vector == 14U &&
            frame->error_code == 0U &&
            frame->cr2 == PAGE_FAULT_TEST_ADDRESS &&
            frame->rip == (uintptr_t)(const void *)interrupt_page_fault_site;
        break;
    case KERNEL_TEST_UNEXPECTED:
        matches = frame->vector == UINT64_C(0x80) && frame->error_code == 0U;
        break;
    default:
        return false;
    }

    if (!matches) {
        kernel_test_fail("fatal interrupt did not match its expectation");
    }

    kernel_test_pass();
}

const char *kernel_test_scenario_name(enum kernel_test_scenario scenario)
{
    switch (scenario) {
    case KERNEL_TEST_NONE:
        return "none";
    case KERNEL_TEST_NORMAL:
        return "normal";
    case KERNEL_TEST_BREAKPOINT:
        return "breakpoint";
    case KERNEL_TEST_INVALID_OPCODE:
        return "invalid-opcode";
    case KERNEL_TEST_PAGE_FAULT:
        return "page-fault";
    case KERNEL_TEST_IST:
        return "ist";
    case KERNEL_TEST_PIT:
        return "pit";
    case KERNEL_TEST_UNEXPECTED:
        return "unexpected";
    case KERNEL_TEST_DOUBLE_FAULT:
        return "double-fault";
    case KERNEL_TEST_APIC:
        return "apic";
    case KERNEL_TEST_IOAPIC:
        return "ioapic";
    case KERNEL_TEST_RETIRED:
        return "retired";
    case KERNEL_TEST_APIC_TIMER:
        return "apic-timer";
    case KERNEL_TEST_TSC:
        return "tsc";
    case KERNEL_TEST_PM_TIMER:
        return "pm-timer";
    case KERNEL_TEST_PIT_RETIRED:
        return "pit-retired";
    case KERNEL_TEST_TIMERS:
        return "timers";
    case KERNEL_TEST_INVALID:
        return "invalid";
    default:
        return "unknown";
    }
}

_Noreturn void kernel_test_fail(const char *reason)
{
    console_write("ST FAIL ");
    console_write(kernel_test_scenario_name(active_scenario));
    console_write(": ");
    console_write(reason);
    console_putc('\n');
    cpu_out32(QEMU_EXIT_PORT, QEMU_FAILURE_VALUE);
    console_halt();
}
