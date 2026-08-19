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
#include <seneri/heap.h>
#include <seneri/interrupts.h>
#include <seneri/ioapic.h>
#include <seneri/memory.h>
#include <seneri/paging.h>
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
 * Eight level-triggered deliveries at 100 Hz. The failure this scenario exists
 * to catch is a pin that delivers once and stops, so one delivery would prove
 * nothing; eight of them cannot happen by accident. The bound is 25 times the
 * 80 ms they should take, so a line that dies is a named status rather than a
 * hang, and it stays well inside one wrap of the reference counter.
 */
#define IOAPIC_LEVEL_TEST_TICKS UINT64_C(8)
#define IOAPIC_LEVEL_TEST_BOUND_NS UINT64_C(2000000000)

/*
 * Intel SDM volume 3A section 4.7 defines the page-fault error code: bit 0 is
 * P, bit 1 is W/R and bit 2 is U/S. A supervisor write to a present read-only
 * page is therefore P=1 W=1 U=0. That is what distinguishes this scenario's
 * fault from the page-fault scenario's absent page, which is P=0 W=0 U=0.
 */
#define PAGING_TEST_FAULT_ERROR_CODE UINT64_C(0x03)
#define PAGING_TEST_PATTERN UINT8_C(0x5A)

/* Enough repetitions that one leaked table per cycle is unmistakable. */
#define PAGING_TEST_CYCLES 64U

/* An address inside the bulk 2 MiB identity map, above the linked image. */
#define PAGING_TEST_HUGE_ADDRESS UINT64_C(0x0000000000A00000)

/*
 * A supervisor write to an absent page is P=0 W=1 U=0. That is a third distinct
 * error code: the page-fault scenario reads an absent page at P=0 W=0 U=0, and
 * the paging scenario writes a present read-only page at P=1 W=1 U=0. No two of
 * the three scenarios can pass on each other's fault.
 */
#define HEAP_TEST_FAULT_ERROR_CODE UINT64_C(0x02)
#define HEAP_TEST_PATTERN UINT8_C(0xC3)

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

    if (token_equals(value, length, "ioapic-level")) {
        return KERNEL_TEST_IOAPIC_LEVEL;
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

    if (token_equals(value, length, "paging")) {
        return KERNEL_TEST_PAGING;
    }

    if (token_equals(value, length, "heap")) {
        return KERNEL_TEST_HEAP;
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
    case KERNEL_TEST_PAGING:
        return UINT8_C(0x20);
    case KERNEL_TEST_HEAP:
        return UINT8_C(0x21);
    case KERNEL_TEST_IOAPIC_LEVEL:
        return UINT8_C(0x22);
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
 * Prove a level-triggered redirection entry delivers more than once.
 *
 * Every other route in this kernel is edge triggered, and an edge needs no
 * acknowledgement at the I/O APIC: the pin is sampled on a transition and
 * nothing is latched. A level-triggered entry latches remote IRR when it
 * delivers and cannot deliver again until an end of interrupt directed at the
 * I/O APIC clears it, so the failure worth hunting is a line that fires exactly
 * once and then goes quiet. One delivery cannot tell that apart from success,
 * which is why this counts eight.
 *
 * The opposite failure is just as silent. Acknowledging a pin whose source is
 * still asserting re-delivers immediately, so a route that never quiets its
 * device counts its eight interrupts in microseconds and looks perfect. The
 * scenario therefore measures how long the eight took as well as that they
 * arrived, and holds it to the interval eight ticks of a 100 Hz timer take.
 */
static void ioapic_level_scenario(void)
{
    struct ioapic_redirection entry;
    struct ioapic_state before;
    struct ioapic_state after;
    uint64_t elapsed_ns = 0U;
    const uint64_t expected_ns = IOAPIC_LEVEL_TEST_TICKS * UINT64_C(1000000000) /
        PIT_TEST_FREQUENCY;

    if (!ioapic_is_initialized() || ioapic_get_state().count == 0U) {
        kernel_test_fail("I/O APIC is not initialized");
    }

    if (pic_mask_snapshot() != UINT16_C(0xFFFF)) {
        kernel_test_fail("a legacy PIC line was left unmasked");
    }

    /* Nothing is routed yet, so every acknowledgement is refused by name. */
    if (ioapic_send_eoi(INTERRUPT_IOAPIC_BASE) !=
            IOAPIC_STATUS_VECTOR_NOT_ROUTED ||
        ioapic_send_eoi(INTERRUPT_IOAPIC_BASE - 1U) !=
            IOAPIC_STATUS_BAD_VECTOR ||
        ioapic_send_eoi(INTERRUPT_LOCAL_APIC_BASE) !=
            IOAPIC_STATUS_BAD_VECTOR ||
        ioapic_read_redirection(INTERRUPT_IOAPIC_BASE, NULL) !=
            IOAPIC_STATUS_NULL_ARGUMENT ||
        ioapic_read_redirection(INTERRUPT_IOAPIC_BASE, &entry) !=
            IOAPIC_STATUS_VECTOR_NOT_ROUTED) {
        kernel_test_fail("an unrouted vector was acknowledged");
    }

    /* And every malformed routing request, through the public interface. */
    if (ioapic_route_isa_irq_as(0U, INTERRUPT_IOAPIC_BASE, 0U,
            (enum ioapic_trigger)7) != IOAPIC_STATUS_BAD_TRIGGER ||
        ioapic_route_isa_irq_as(0U, INTERRUPT_IOAPIC_BASE, UINT8_MAX + 1U,
            IOAPIC_TRIGGER_FORCE_LEVEL) != IOAPIC_STATUS_BAD_DESTINATION ||
        ioapic_route_isa_irq_as(UINT8_C(16), INTERRUPT_IOAPIC_BASE, 0U,
            IOAPIC_TRIGGER_FORCE_LEVEL) != IOAPIC_STATUS_BAD_IRQ) {
        kernel_test_fail("a malformed routing request was accepted");
    }

    /* A wait needs a running timer, a target and a bound the clock can hold. */
    if (pit_wait_for_ticks_bounded(1U, IOAPIC_LEVEL_TEST_BOUND_NS, NULL) !=
            PIT_STATUS_NULL_ARGUMENT ||
        pit_wait_for_ticks_bounded(1U, IOAPIC_LEVEL_TEST_BOUND_NS,
            &elapsed_ns) != PIT_STATUS_NOT_RUNNING ||
        elapsed_ns != 0U) {
        kernel_test_fail("a bounded wait ran without a running timer");
    }

    if (pit_start(PIT_TEST_FREQUENCY, PIT_ROUTE_IO_APIC_LEVEL) !=
        PIT_STATUS_OK) {
        kernel_test_fail("the timer would not take the level-triggered route");
    }

    if (pit_active_route() != PIT_ROUTE_IO_APIC_LEVEL ||
        pit_wait_for_ticks_bounded(0U, IOAPIC_LEVEL_TEST_BOUND_NS,
            &elapsed_ns) != PIT_STATUS_BAD_INTERVAL ||
        pit_wait_for_ticks_bounded(1U, PIT_MAX_WAIT_NS + 1U, &elapsed_ns) !=
            PIT_STATUS_BAD_INTERVAL) {
        kernel_test_fail("a bounded wait accepted an interval it cannot hold");
    }

    /*
     * Read the entry off the hardware. An entry programmed edge triggered while
     * Seneri's records called it level triggered would deliver every interrupt
     * below and latch nothing, so this is the check that catches it.
     */
    if (ioapic_read_redirection(pit_active_vector(), &entry) !=
            IOAPIC_STATUS_OK ||
        !entry.level_triggered || entry.masked || entry.active_low ||
        entry.vector != pit_active_vector() ||
        entry.global_interrupt != 2U) {
        kernel_test_fail("the level route did not read back level triggered");
    }

    if (!ioapic_vector_is_level_triggered(pit_active_vector()) ||
        ioapic_get_state().level_routes != 1U) {
        kernel_test_fail("the level route was not recorded as level triggered");
    }

    if (pic_mask_snapshot() != UINT16_C(0xFFFF)) {
        kernel_test_fail("level routing unmasked a legacy PIC line");
    }

    /*
     * A vector names one pin. Pointing this one at IRQ4's pin as well would
     * leave the timer's entry unmasked and delivering a vector the dispatcher
     * would acknowledge on the wrong unit, so it is refused by name.
     */
    if (ioapic_route_isa_irq(4U, pit_active_vector(), 0U) !=
        IOAPIC_STATUS_VECTOR_IN_USE) {
        kernel_test_fail("one vector was pointed at two redirection entries");
    }

    before = ioapic_get_state();

    if (pit_wait_for_ticks_bounded(
            IOAPIC_LEVEL_TEST_TICKS,
            IOAPIC_LEVEL_TEST_BOUND_NS,
            &elapsed_ns
        ) != PIT_STATUS_OK) {
        kernel_test_fail("the level-triggered line stopped delivering");
    }

    after = ioapic_get_state();
    console_write("ST INFO ioapic-level: ");
    console_write_u64(pit_ticks());
    console_write(" deliveries, remote IRR ");
    console_write_u64(after.remote_irr_observed);
    console_write(", directed EOI ");
    console_write_u64(after.directed_eoi_count);
    console_write(", mode ");
    console_write(after.directed_eoi_mode ? "directed" : "broadcast");
    console_write(", in ");
    console_write_u64(elapsed_ns);
    console_write(" ns\n");

    if (pit_ticks() < IOAPIC_LEVEL_TEST_TICKS) {
        kernel_test_fail("a level-triggered line delivered too few interrupts");
    }

    /*
     * And not far more than it was asked for. A pin acknowledged while its
     * source is still asserting re-delivers inside the acknowledgement, so it
     * counts thousands of interrupts in the time eight should take. The
     * interval check below would fail on that too, but only after describing
     * it as a timing problem; this names it for what it is.
     */
    if (pit_ticks() > IOAPIC_LEVEL_TEST_TICKS * 2U) {
        kernel_test_fail("a level-triggered line delivered without stopping");
    }

    /*
     * Every delivery latched remote IRR and every one was acknowledged at the
     * I/O APIC. The counts are what say the entry behaved as a level-triggered
     * entry rather than merely that interrupts arrived.
     */
    if (after.remote_irr_observed - before.remote_irr_observed <
            IOAPIC_LEVEL_TEST_TICKS ||
        after.remote_irr_missing != 0U ||
        (after.directed_eoi_mode &&
         (after.directed_eoi_count - before.directed_eoi_count <
              IOAPIC_LEVEL_TEST_TICKS ||
          !apic_get_state().eoi_broadcasts_suppressed)) ||
        (!after.directed_eoi_mode &&
         after.directed_eoi_count != before.directed_eoi_count)) {
        kernel_test_fail("a level-triggered delivery did not latch remote IRR");
    }

    /* And they took the time eight ticks take, rather than no time at all. */
    if (!pm_timer_durations_agree(
            elapsed_ns,
            expected_ns,
            PM_TIMER_TOLERANCE_QUARTER
        )) {
        kernel_test_fail("level-triggered deliveries did not take a period");
    }

    if (pit_stop() != PIT_STATUS_OK) {
        kernel_test_fail("the level route would not stop");
    }

    /* Stopping unroutes the entry, so its vector is nothing's again. */
    if (ioapic_vector_is_level_triggered(pit_active_vector()) ||
        ioapic_get_state().level_routes != 0U ||
        ioapic_read_redirection(pit_active_vector(), &entry) !=
            IOAPIC_STATUS_VECTOR_NOT_ROUTED) {
        kernel_test_fail("a stopped level route is still routed");
    }

    /*
     * The same pin, sampled as an edge again. An edge-triggered entry has no
     * remote IRR to latch and nothing to acknowledge, so both counters must
     * stand still across eight more deliveries. Without this, an
     * implementation that treated every route as level triggered would pass
     * everything above.
     */
    before = ioapic_get_state();

    if (pit_start(PIT_TEST_FREQUENCY, PIT_ROUTE_IO_APIC) != PIT_STATUS_OK ||
        ioapic_vector_is_level_triggered(pit_active_vector()) ||
        ioapic_send_eoi(pit_active_vector()) !=
            IOAPIC_STATUS_NOT_LEVEL_TRIGGERED) {
        kernel_test_fail("an edge route was treated as level triggered");
    }

    if (pit_wait_for_ticks_bounded(
            IOAPIC_LEVEL_TEST_TICKS,
            IOAPIC_LEVEL_TEST_BOUND_NS,
            &elapsed_ns
        ) != PIT_STATUS_OK) {
        kernel_test_fail("the edge-triggered line stopped delivering");
    }

    after = ioapic_get_state();

    if (after.remote_irr_observed != before.remote_irr_observed ||
        after.directed_eoi_count != before.directed_eoi_count ||
        after.level_routes != 0U) {
        kernel_test_fail("an edge-triggered delivery latched remote IRR");
    }

    if (pit_stop() != PIT_STATUS_OK) {
        kernel_test_fail("the edge route would not stop");
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
    size_t heap_live_before;
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

    /*
     * The deadline table is a heap allocation now, not a static array, so
     * starting must take exactly one block and report the capacity it got.
     */
    heap_live_before = heap_get_state().live_allocations;

    if (timer_capacity() != 0U) {
        kernel_test_fail("deadline timers held a table before starting");
    }

    if (timer_start() != TIMER_STATUS_OK) {
        kernel_test_fail("deadline timers would not start");
    }

    if (timer_capacity() != TIMER_MAX_PENDING ||
        heap_get_state().live_allocations != heap_live_before + 1U) {
        kernel_test_fail("starting did not take one table from the heap");
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

    /*
     * And stopping gives it back. A subsystem that took heap memory once per
     * start and never returned it would look perfectly correct in every other
     * check here and exhaust the heap over a long-running kernel.
     */
    if (timer_capacity() != 0U ||
        heap_get_state().live_allocations != heap_live_before) {
        kernel_test_fail("stopping did not return the table to the heap");
    }

    if (heap_verify() != HEAP_STATUS_OK) {
        kernel_test_fail("the heap did not survive the deadline table");
    }

    if (apic_spurious_count() != 0U) {
        kernel_test_fail("local APIC raised a spurious interrupt");
    }
}

/*
 * Written through a volatile pointer inside the scenario and read back after a
 * permission change, so the compiler cannot cache either side of it or assume
 * it knows what a page it never mapped contains.
 */
static volatile uint8_t paging_scratch;

/* Larger than early boot should place on the 16 KiB kernel stack. */
static struct acpi_topology paging_probe_topology;

/*
 * Prove the permissions are enforced by the processor rather than merely
 * recorded in a table.
 *
 * `make verify` has always refused an RWX load segment, and until this
 * increment that assertion was the only thing standing behind Seneri's W^X
 * claim - and it inspects the ELF file, not the machine the kernel runs on.
 * Everything below the rejections is the part a file check can never do: a
 * fresh frame is mapped writable, written, narrowed to read-only, and written
 * again, and the scenario passes only if the processor refuses the second write
 * with the exact fault a supervisor write to a present read-only page produces.
 *
 * The probe returns if the store succeeds, so a permission that quietly failed
 * to take shows up as a scenario failure rather than as a timeout.
 */
static void paging_scenario(void)
{
    volatile uint8_t *probe =
        (volatile uint8_t *)(uintptr_t)PAGING_PROBE_ADDRESS;
    const uint64_t text = (uint64_t)(uintptr_t)(const void *)
        paging_probe_write_site;
    const uint64_t data = (uint64_t)(uintptr_t)(const void *)&paging_scratch;
    struct paging_translation translation;
    struct paging_state paging;
    struct paging_audit audit;
    size_t frames_before;
    size_t tables_before;
    uintptr_t frame;

    if (!paging_is_active()) {
        kernel_test_fail("kernel page tables are not installed");
    }

    paging = paging_get_state();

    if (!paging.no_execute_active || !paging.write_protect_active) {
        kernel_test_fail("W^X is not enforceable on this processor");
    }

    if (paging.root_physical_address == 0U || paging.table_frames == 0U ||
        paging_verify() != PAGING_STATUS_OK) {
        kernel_test_fail("installed page tables do not match their intent");
    }

    /* The whole point of the increment, read back off the live hierarchy. */
    if (paging_audit_hierarchy(&audit) != PAGING_STATUS_OK ||
        audit.leaf_count == 0U || audit.executable_leaves == 0U ||
        audit.write_execute_leaves != 0U || audit.user_leaves != 0U) {
        kernel_test_fail("a live mapping is writable and executable");
    }

    if (paging_translate(text, &translation) != PAGING_STATUS_OK ||
        translation.permissions != PAGING_EXECUTE ||
        translation.physical_address != text) {
        kernel_test_fail("kernel text is not read-only and executable");
    }

    if (paging_translate(data, &translation) != PAGING_STATUS_OK ||
        translation.permissions != PAGING_WRITE ||
        translation.physical_address != data) {
        kernel_test_fail("kernel data is not writable and non-executable");
    }

    /* The null page is absent, so a null dereference cannot read low memory. */
    if (paging_translate(0U, &translation) != PAGING_STATUS_NOT_MAPPED ||
        translation.level != 0U) {
        kernel_test_fail("the null page is mapped");
    }

    /* Every refusal, through the public interface, against the live tables. */
    if (paging_map(PAGING_PROBE_ADDRESS + 1U, 0U, SENERI_PAGE_SIZE,
            PAGING_WRITE) != PAGING_STATUS_UNALIGNED_ADDRESS ||
        paging_map(PAGING_PROBE_ADDRESS, 0U, 0U, PAGING_WRITE) !=
            PAGING_STATUS_ZERO_LENGTH ||
        paging_map(UINT64_C(0x0000800000000000), 0U, SENERI_PAGE_SIZE,
            PAGING_WRITE) != PAGING_STATUS_NONCANONICAL_ADDRESS ||
        paging_map(PAGING_PROBE_ADDRESS, 0U, SENERI_PAGE_SIZE,
            PAGING_WRITE | PAGING_EXECUTE) !=
            PAGING_STATUS_WRITABLE_AND_EXECUTABLE) {
        kernel_test_fail("a malformed mapping request was accepted");
    }

    if (paging_map(text & ~(SENERI_PAGE_SIZE - 1U), 0U, SENERI_PAGE_SIZE,
            PAGING_WRITE) != PAGING_STATUS_ALREADY_MAPPED ||
        paging_unmap(PAGING_PROBE_ADDRESS, SENERI_PAGE_SIZE) !=
            PAGING_STATUS_NOT_MAPPED ||
        paging_protect(PAGING_PROBE_ADDRESS, SENERI_PAGE_SIZE, PAGING_READ) !=
            PAGING_STATUS_NOT_MAPPED) {
        kernel_test_fail("an impossible mapping change was accepted");
    }

    /*
     * The bulk identity map uses 2 MiB leaves, and splitting one is deferred,
     * so a 4 KiB change inside one is refused rather than silently applied to
     * the whole 2 MiB.
     */
    if (paging_protect(PAGING_TEST_HUGE_ADDRESS, SENERI_PAGE_SIZE,
            PAGING_READ) != PAGING_STATUS_HUGE_PAGE_PRESENT ||
        paging_unmap(PAGING_TEST_HUGE_ADDRESS, SENERI_PAGE_SIZE) !=
            PAGING_STATUS_HUGE_PAGE_PRESENT) {
        kernel_test_fail("a 2 MiB mapping accepted a 4 KiB change");
    }

    if (paging_initialize(&paging_probe_topology) !=
        PAGING_STATUS_ALREADY_INITIALIZED) {
        kernel_test_fail("page tables accepted a second installation");
    }

    if (frame_allocate(&frame) != FRAME_STATUS_OK) {
        kernel_test_fail("no frame was available for the probe page");
    }

    if (paging_map(PAGING_PROBE_ADDRESS, frame, SENERI_PAGE_SIZE,
            PAGING_WRITE) != PAGING_STATUS_OK ||
        paging_map(PAGING_PROBE_ADDRESS, frame, SENERI_PAGE_SIZE,
            PAGING_WRITE) != PAGING_STATUS_ALREADY_MAPPED) {
        kernel_test_fail("the probe page would not map exactly once");
    }

    *probe = PAGING_TEST_PATTERN;

    if (*probe != PAGING_TEST_PATTERN) {
        kernel_test_fail("a writable mapping did not hold a write");
    }

    if (paging_translate(PAGING_PROBE_ADDRESS, &translation) !=
            PAGING_STATUS_OK ||
        translation.physical_address != (uint64_t)frame ||
        translation.permissions != PAGING_WRITE ||
        translation.level != 1U) {
        kernel_test_fail("the probe page does not translate to its frame");
    }

    if (paging_protect(PAGING_PROBE_ADDRESS, SENERI_PAGE_SIZE, PAGING_READ) !=
        PAGING_STATUS_OK) {
        kernel_test_fail("the probe page would not narrow to read-only");
    }

    if (paging_translate(PAGING_PROBE_ADDRESS, &translation) !=
            PAGING_STATUS_OK ||
        translation.permissions != PAGING_READ ||
        translation.physical_address != (uint64_t)frame) {
        kernel_test_fail("narrowing a mapping changed what it points at");
    }

    /* Reading is still permitted, and the contents survived the change. */
    if (*probe != PAGING_TEST_PATTERN) {
        kernel_test_fail("a read-only mapping lost the page contents");
    }

    /*
     * Map and undo the same page many times over. One leaked interior table
     * per cycle is invisible in a single pass and fatal over a long-running
     * kernel, so the check that matters is that the frame count is identical
     * after sixty-four cycles - and the paging state's own table count with it.
     */
    if (paging_unmap(PAGING_PROBE_ADDRESS, SENERI_PAGE_SIZE) !=
        PAGING_STATUS_OK) {
        kernel_test_fail("the probe page would not unmap before the cycle");
    }

    frames_before = frame_allocator_get_stats().free_frames;
    tables_before = paging_get_state().table_frames;

    for (size_t cycle = 0; cycle < PAGING_TEST_CYCLES; ++cycle) {
        uintptr_t cycle_frame;

        if (frame_allocate(&cycle_frame) != FRAME_STATUS_OK ||
            paging_map(PAGING_PROBE_ADDRESS, cycle_frame, SENERI_PAGE_SIZE,
                PAGING_WRITE) != PAGING_STATUS_OK ||
            paging_unmap(PAGING_PROBE_ADDRESS, SENERI_PAGE_SIZE) !=
                PAGING_STATUS_OK ||
            frame_release(cycle_frame) != FRAME_STATUS_OK) {
            kernel_test_fail("a map and unmap cycle did not complete");
        }
    }

    if (frame_allocator_get_stats().free_frames != frames_before) {
        kernel_test_fail("repeated mapping leaked a physical frame");
    }

    if (paging_get_state().table_frames != tables_before) {
        kernel_test_fail("repeated mapping leaked a page table");
    }

    if (paging_verify() != PAGING_STATUS_OK) {
        kernel_test_fail("the hierarchy did not survive repeated mapping");
    }

    /* Put the probe page back so the fault below has something to narrow. */
    if (frame_allocate(&frame) != FRAME_STATUS_OK ||
        paging_map(PAGING_PROBE_ADDRESS, frame, SENERI_PAGE_SIZE,
            PAGING_WRITE) != PAGING_STATUS_OK ||
        paging_protect(PAGING_PROBE_ADDRESS, SENERI_PAGE_SIZE, PAGING_READ) !=
            PAGING_STATUS_OK) {
        kernel_test_fail("the probe page would not come back read-only");
    }

    console_write("ST INFO paging: read-only write to ");
    console_write_hex(PAGING_PROBE_ADDRESS);
    console_write(" expecting P=1 W=1 U=0\n");

    /*
     * The store that must fault. If the processor takes it, control returns
     * here and kernel_test_run reports the failure; if it faults,
     * kernel_test_handle_fatal_interrupt matches the vector, the error code,
     * CR2 and the faulting instruction, and passes.
     */
    paging_probe_write(probe, (uint8_t)~PAGING_TEST_PATTERN);
}

/*
 * Prove the heap hands out memory that is actually distinct, that it refuses
 * everything it should, and that its guard pages are enforced by the processor.
 *
 * The refusals matter more here than anywhere else in the kernel. An allocator
 * that accepts a pointer it never returned will happily mark a live block free
 * and hand the same bytes to two callers, and nothing downstream can detect
 * that. So every wrong pointer this scenario can construct - interior, below
 * the window, above the window, unaligned, already freed - is checked by name.
 */
static void heap_scenario(void)
{
    volatile uint8_t *bytes;
    struct paging_translation translation;
    struct heap_state heap;
    uint64_t committed_before;
    size_t pages_before;
    enum heap_status status;
    void *first = NULL;
    void *second = NULL;
    void *third = NULL;

    if (!heap_is_active() || !paging_is_active()) {
        kernel_test_fail("kernel heap is not online");
    }

    if (heap_initialize() != HEAP_STATUS_ALREADY_INITIALIZED) {
        kernel_test_fail("heap accepted a second initialization");
    }

    status = heap_verify();

    if (status != HEAP_STATUS_OK) {
        kernel_test_fail(heap_status_string(status));
    }

    /* Malformed requests, each refused by its own name. */
    if (heap_allocate(0U, &first) != HEAP_STATUS_ZERO_SIZE || first != NULL ||
        heap_allocate(HEAP_SIZE + 1U, &first) != HEAP_STATUS_TOO_LARGE ||
        first != NULL ||
        heap_allocate(16U, NULL) != HEAP_STATUS_NULL_ARGUMENT) {
        kernel_test_fail("a malformed allocation request was accepted");
    }

    if (heap_allocate(64U, &first) != HEAP_STATUS_OK || first == NULL ||
        heap_allocate(64U, &second) != HEAP_STATUS_OK || second == NULL ||
        first == second) {
        kernel_test_fail("the heap would not produce two distinct blocks");
    }

    if (((uint64_t)(uintptr_t)first & (HEAP_ALIGNMENT - 1U)) != 0U ||
        ((uint64_t)(uintptr_t)second & (HEAP_ALIGNMENT - 1U)) != 0U) {
        kernel_test_fail("the heap returned a misaligned allocation");
    }

    /* Both blocks must lie inside the window, between the guards. */
    if ((uint64_t)(uintptr_t)first < HEAP_BASE ||
        (uint64_t)(uintptr_t)second >= HEAP_GUARD_ABOVE) {
        kernel_test_fail("the heap returned a block outside its window");
    }

    /* Every wrong pointer this scenario can construct. */
    if (heap_free(NULL) != HEAP_STATUS_NULL_ARGUMENT ||
        heap_free((void *)((uintptr_t)first + 1U)) !=
            HEAP_STATUS_BAD_POINTER ||
        heap_free((void *)((uintptr_t)first + HEAP_ALIGNMENT)) !=
            HEAP_STATUS_BAD_POINTER ||
        heap_free((void *)(uintptr_t)HEAP_GUARD_BELOW) !=
            HEAP_STATUS_BAD_POINTER ||
        heap_free((void *)(uintptr_t)HEAP_GUARD_ABOVE) !=
            HEAP_STATUS_BAD_POINTER) {
        kernel_test_fail("the heap accepted a pointer it never returned");
    }

    if (heap_free(first) != HEAP_STATUS_OK ||
        heap_free(first) != HEAP_STATUS_DOUBLE_FREE) {
        kernel_test_fail("the heap accepted a double free");
    }

    /*
     * The freed block is the best fit for the same size again, so a heap that
     * reuses its free space hands back the identical address. A heap that only
     * ever grew would return something new here and slowly exhaust the window.
     */
    if (heap_allocate(64U, &third) != HEAP_STATUS_OK || third != first) {
        kernel_test_fail("the heap did not reuse a freed block");
    }

    bytes = (volatile uint8_t *)third;

    for (uint64_t index = 0; index < 64U; ++index) {
        bytes[index] = HEAP_TEST_PATTERN;
    }

    for (uint64_t index = 0; index < 64U; ++index) {
        if (bytes[index] != HEAP_TEST_PATTERN) {
            kernel_test_fail("a heap block did not hold what was written");
        }
    }

    if (heap_free(third) != HEAP_STATUS_OK ||
        heap_free(second) != HEAP_STATUS_OK) {
        kernel_test_fail("the heap refused to release its own allocation");
    }

    heap = heap_get_state();

    if (heap.live_allocations != 0U || heap.allocated_bytes != 0U ||
        heap.block_count != 1U) {
        kernel_test_fail("the heap did not coalesce back to one free block");
    }

    status = heap_verify();

    if (status != HEAP_STATUS_OK) {
        kernel_test_fail(heap_status_string(status));
    }

    /*
     * Ask for the entire window in one allocation. Which way this goes depends
     * on the machine, and both ways are worth checking, so the scenario asks
     * what happened rather than assuming how much memory it has.
     *
     * With more free memory than the window, growth commits every page and the
     * next byte requested must be refused at the window bound. With less, the
     * growth runs out of frames part way through, which is the only path that
     * exercises rollback - and then what matters is that the heap is left
     * exactly as it was, with every page that had been mapped given back.
     */
    committed_before = heap.committed_bytes;
    pages_before = heap.mapped_pages;
    status = heap_allocate(HEAP_SIZE, &first);

    if (status == HEAP_STATUS_OUT_OF_MEMORY) {
        heap = heap_get_state();

        if (first != NULL || heap_verify() != HEAP_STATUS_OK ||
            heap.committed_bytes != committed_before ||
            heap.mapped_pages != pages_before ||
            heap.live_allocations != 0U) {
            kernel_test_fail("a failed heap growth did not roll back");
        }

        console_write("ST INFO heap: growth rolled back at the frame limit\n");
    } else if (status == HEAP_STATUS_OK) {
        heap = heap_get_state();

        if (first == NULL || heap.committed_bytes != HEAP_SIZE ||
            heap.mapped_pages != HEAP_SIZE / PAGING_PAGE_SIZE) {
            kernel_test_fail("committing the window did not map every page");
        }

        if (heap_allocate(HEAP_ALIGNMENT, &second) !=
                HEAP_STATUS_OUT_OF_MEMORY ||
            second != NULL) {
            kernel_test_fail("a full heap accepted another allocation");
        }

        /* The guards survive the window being fully committed against them. */
        if (heap_verify() != HEAP_STATUS_OK ||
            heap_free(first) != HEAP_STATUS_OK ||
            heap_verify() != HEAP_STATUS_OK) {
            kernel_test_fail("heap invariants do not hold at full commitment");
        }
    } else {
        kernel_test_fail(heap_status_string(status));
    }

    /*
     * The guard page above the window is absent and stays absent. Its address
     * is one past the last byte the heap can ever hand out, so this is exactly
     * the write a caller running off the end of its last allocation would make.
     */
    if (paging_translate(HEAP_GUARD_ABOVE, &translation) !=
        PAGING_STATUS_NOT_MAPPED) {
        kernel_test_fail("the upper heap guard page is mapped");
    }

    console_write("ST INFO heap: guard write to ");
    console_write_hex(HEAP_GUARD_ABOVE);
    console_write(" expecting P=0 W=1 U=0\n");

    paging_probe_write(
        (volatile uint8_t *)(uintptr_t)HEAP_GUARD_ABOVE,
        HEAP_TEST_PATTERN
    );
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
    case KERNEL_TEST_IOAPIC_LEVEL:
        ioapic_level_scenario();
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
    case KERNEL_TEST_PAGING:
        paging_scenario();
        kernel_test_fail("a read-only page accepted a supervisor write");
    case KERNEL_TEST_HEAP:
        heap_scenario();
        kernel_test_fail("a heap guard page accepted a supervisor write");
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
    case KERNEL_TEST_PAGING:
        matches = frame->vector == 14U &&
            frame->error_code == PAGING_TEST_FAULT_ERROR_CODE &&
            frame->cr2 == PAGING_PROBE_ADDRESS &&
            frame->rip == (uintptr_t)(const void *)paging_probe_write_site;
        break;
    case KERNEL_TEST_HEAP:
        matches = frame->vector == 14U &&
            frame->error_code == HEAP_TEST_FAULT_ERROR_CODE &&
            frame->cr2 == HEAP_GUARD_ABOVE &&
            frame->rip == (uintptr_t)(const void *)paging_probe_write_site;
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
    case KERNEL_TEST_IOAPIC_LEVEL:
        return "ioapic-level";
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
    case KERNEL_TEST_PAGING:
        return "paging";
    case KERNEL_TEST_HEAP:
        return "heap";
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
