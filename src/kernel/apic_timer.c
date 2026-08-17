/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <seneri/apic.h>
#include <seneri/apic_timer.h>
#include <seneri/cpu.h>
#include <seneri/interrupts.h>
#include <seneri/pm_timer.h>

/* Intel SDM volume 3A table 11-1 places the timer registers at these offsets. */
#define APIC_REGISTER_LVT_TIMER UINT32_C(0x0320)
#define APIC_REGISTER_TIMER_INITIAL_COUNT UINT32_C(0x0380)
#define APIC_REGISTER_TIMER_CURRENT_COUNT UINT32_C(0x0390)
#define APIC_REGISTER_TIMER_DIVIDE UINT32_C(0x03E0)

/* Intel SDM volume 3A figure 11-10 defines the timer's LVT and divide fields. */
#define APIC_TIMER_MODE_PERIODIC UINT32_C(0x00020000)
#define APIC_TIMER_MASKED UINT32_C(0x00010000)
#define APIC_TIMER_LVT_WRITABLE_MASK UINT32_C(0x000707FF)

/*
 * Divide configuration 0b0011 selects divide by sixteen. A slower count keeps a
 * one-shot calibration run from exhausting the 32-bit counter, and still leaves
 * far more resolution than the reference interval needs.
 */
#define APIC_TIMER_DIVIDE_BY_SIXTEEN UINT32_C(0x00000003)
#define APIC_TIMER_DIVISOR 16U
#define APIC_TIMER_MAX_COUNT UINT32_C(0xFFFFFFFF)

/*
 * The reference interval: a tenth of a second of the ACPI power management
 * timer, whose rate the ACPI specification fixes rather than one this kernel
 * measured. The PIT used to serve here, and its own tick accounting was wrong by
 * a factor of two for as long as it did; see docs/PIT_RETIREMENT.md.
 */
#define REFERENCE_PM_TICKS (PM_TIMER_FREQUENCY_HZ / 10U)

/*
 * The countdown is a 32-bit register, so the widest span the rate arithmetic can
 * be handed is UINT32_MAX. Scaling that by the reference rate must not overflow;
 * proving it here removes the need for a guard no test could reach.
 */
_Static_assert(UINT64_MAX / PM_TIMER_FREQUENCY_HZ >= UINT32_MAX,
               "an APIC timer countdown scaled by the PM timer rate overflows");

static volatile uint64_t tick_counter __attribute__((aligned(8)));
static uint64_t counts_per_second;
static uint32_t configured_frequency;
static bool calibrated;
static bool running;

static void apic_timer_handler(struct interrupt_frame *frame, void *context)
{
    (void)frame;
    (void)context;
    ++tick_counter;
}

/*
 * Turn one observed countdown into a rate. The counter runs down, so a start
 * that is not above the end means it never counted, and an end of zero means it
 * reached the bottom and the elapsed count is a floor rather than a measurement.
 */
static enum apic_timer_status rate_from_countdown(
    uint32_t start,
    uint32_t end,
    uint32_t reference_frequency,
    uint64_t reference_ticks,
    uint64_t *rate
)
{
    uint64_t elapsed;

    if (reference_frequency == 0U || reference_ticks == 0U) {
        return APIC_TIMER_STATUS_BAD_FREQUENCY;
    }

    if (end == 0U) {
        return APIC_TIMER_STATUS_COUNTER_EXHAUSTED;
    }

    if (start <= end) {
        return APIC_TIMER_STATUS_NOT_COUNTING;
    }

    elapsed = (uint64_t)start - (uint64_t)end;
    *rate = elapsed * reference_frequency / reference_ticks;

    if (*rate == 0U) {
        return APIC_TIMER_STATUS_NOT_COUNTING;
    }

    return APIC_TIMER_STATUS_OK;
}

static enum apic_timer_status count_for_frequency(
    uint64_t rate,
    uint32_t frequency_hz,
    uint32_t *count
)
{
    uint64_t requested;

    if (frequency_hz == 0U || rate == 0U) {
        return APIC_TIMER_STATUS_BAD_FREQUENCY;
    }

    requested = rate / frequency_hz;

    if (requested == 0U || requested > APIC_TIMER_MAX_COUNT) {
        return APIC_TIMER_STATUS_BAD_FREQUENCY;
    }

    *count = (uint32_t)requested;
    return APIC_TIMER_STATUS_OK;
}

static void stop_counting(void)
{
    apic_register_write(APIC_REGISTER_TIMER_INITIAL_COUNT, 0U);
    apic_register_write(
        APIC_REGISTER_LVT_TIMER,
        APIC_TIMER_MASKED | APIC_SPURIOUS_VECTOR
    );
}

/*
 * Measure the local APIC timer against the ACPI power management timer. The APIC
 * timer's input is the processor's bus or core crystal clock, whose rate Seneri
 * is not told, so the only honest way to use it as a clock is to count it
 * against a reference whose rate is known. The PM timer's rate is stated by
 * the ACPI specification and measured against nothing, which is what makes it
 * a reference rather than another opinion.
 *
 * The span is scaled by the ticks the reference actually advanced, not by the
 * ticks that were requested. A bounded wait may overshoot, and using the
 * observed span keeps the overshoot out of the rate instead of inflating it.
 */
enum apic_timer_status apic_timer_calibrate(void)
{
    uint32_t start;
    uint32_t end;
    uint64_t rate = 0U;
    uint64_t reference_ticks = 0U;
    enum apic_timer_status status;
    enum pm_timer_status pm_status;

    if (!apic_is_online()) {
        return APIC_TIMER_STATUS_APIC_OFFLINE;
    }

    if (cpu_interrupts_enabled()) {
        return APIC_TIMER_STATUS_INTERRUPTS_ENABLED;
    }

    if (calibrated) {
        return APIC_TIMER_STATUS_ALREADY_CALIBRATED;
    }

    if (!pm_timer_is_present()) {
        return APIC_TIMER_STATUS_REFERENCE_FAILURE;
    }

    /* Count down from the top, masked, so calibration raises no interrupt. */
    apic_register_write(APIC_REGISTER_TIMER_DIVIDE, APIC_TIMER_DIVIDE_BY_SIXTEEN);
    apic_register_write(
        APIC_REGISTER_LVT_TIMER,
        APIC_TIMER_MASKED | APIC_SPURIOUS_VECTOR
    );
    apic_register_write(APIC_REGISTER_TIMER_INITIAL_COUNT, APIC_TIMER_MAX_COUNT);
    start = apic_register_read(APIC_REGISTER_TIMER_CURRENT_COUNT);

    pm_status = pm_timer_wait(REFERENCE_PM_TICKS, &reference_ticks);
    end = apic_register_read(APIC_REGISTER_TIMER_CURRENT_COUNT);
    stop_counting();

    if (pm_status != PM_TIMER_STATUS_OK) {
        return APIC_TIMER_STATUS_REFERENCE_FAILURE;
    }

    status = rate_from_countdown(
        start,
        end,
        PM_TIMER_FREQUENCY_HZ,
        reference_ticks,
        &rate
    );

    if (status != APIC_TIMER_STATUS_OK) {
        return status;
    }

    counts_per_second = rate;
    calibrated = true;
    return APIC_TIMER_STATUS_OK;
}

enum apic_timer_status apic_timer_start(uint32_t frequency_hz)
{
    uint32_t count = 0U;
    uint32_t lvt;
    enum apic_timer_status status;
    enum interrupt_status interrupt_status;

    if (!apic_is_online()) {
        return APIC_TIMER_STATUS_APIC_OFFLINE;
    }

    if (cpu_interrupts_enabled()) {
        return APIC_TIMER_STATUS_INTERRUPTS_ENABLED;
    }

    if (!calibrated) {
        return APIC_TIMER_STATUS_NOT_CALIBRATED;
    }

    if (running) {
        return APIC_TIMER_STATUS_ALREADY_RUNNING;
    }

    status = count_for_frequency(counts_per_second, frequency_hz, &count);

    if (status != APIC_TIMER_STATUS_OK) {
        return status;
    }

    interrupt_status = interrupt_register_handler(
        APIC_TIMER_VECTOR,
        apic_timer_handler,
        NULL
    );

    if (interrupt_status != INTERRUPT_STATUS_OK) {
        return APIC_TIMER_STATUS_INTERRUPT_FAILURE;
    }

    tick_counter = 0U;
    lvt = APIC_TIMER_MODE_PERIODIC | APIC_TIMER_VECTOR;
    apic_register_write(APIC_REGISTER_TIMER_DIVIDE, APIC_TIMER_DIVIDE_BY_SIXTEEN);
    apic_register_write(APIC_REGISTER_LVT_TIMER, lvt);

    if ((apic_register_read(APIC_REGISTER_LVT_TIMER) &
         APIC_TIMER_LVT_WRITABLE_MASK) != lvt) {
        stop_counting();
        (void)interrupt_unregister_handler(APIC_TIMER_VECTOR);
        return APIC_TIMER_STATUS_READBACK_MISMATCH;
    }

    /* The count starts the timer, so it is written last. */
    apic_register_write(APIC_REGISTER_TIMER_INITIAL_COUNT, count);
    configured_frequency = frequency_hz;
    running = true;
    return APIC_TIMER_STATUS_OK;
}

enum apic_timer_status apic_timer_stop(void)
{
    enum interrupt_status interrupt_status;

    if (!running) {
        return APIC_TIMER_STATUS_NOT_RUNNING;
    }

    if (cpu_interrupts_enabled()) {
        return APIC_TIMER_STATUS_INTERRUPTS_ENABLED;
    }

    stop_counting();
    interrupt_status = interrupt_unregister_handler(APIC_TIMER_VECTOR);

    if (interrupt_status != INTERRUPT_STATUS_OK) {
        return APIC_TIMER_STATUS_INTERRUPT_FAILURE;
    }

    running = false;
    configured_frequency = 0U;
    return APIC_TIMER_STATUS_OK;
}

enum apic_timer_status apic_timer_wait_for_ticks(uint64_t tick_count)
{
    uint64_t target;

    if (!running) {
        return APIC_TIMER_STATUS_NOT_RUNNING;
    }

    if (cpu_interrupts_enabled()) {
        return APIC_TIMER_STATUS_INTERRUPTS_ENABLED;
    }

    if (tick_count == 0U) {
        return APIC_TIMER_STATUS_OK;
    }

    if (tick_count > UINT64_MAX - tick_counter) {
        target = UINT64_MAX;
    } else {
        target = tick_counter + tick_count;
    }

    while (tick_counter < target) {
        cpu_enable_and_halt();
    }

    cpu_interrupt_disable();
    return APIC_TIMER_STATUS_OK;
}

uint64_t apic_timer_ticks(void)
{
    return tick_counter;
}

uint64_t apic_timer_counts_per_second(void)
{
    return counts_per_second;
}

bool apic_timer_is_calibrated(void)
{
    return calibrated;
}

bool apic_timer_is_running(void)
{
    return running;
}

/*
 * Calibration arithmetic decides what every later interval means, and it is
 * provable without hardware. A wrong rate would not fail loudly; it would make
 * a timer that runs at the wrong speed forever.
 */
bool apic_timer_self_test(void)
{
    uint64_t rate = 0U;
    uint32_t count = 0U;

    /*
     * One full second of the reference is PM_TIMER_FREQUENCY_HZ of its ticks, so
     * a 62.5 MHz count advances 62.5 million times across it. Expressing the
     * interval as exactly one second keeps the expected rate exact rather than
     * truncated, which is what makes this an assertion rather than an estimate.
     */
    if (rate_from_countdown(
            UINT32_C(0xFFFFFFFF),
            UINT32_C(0xFFFFFFFF) - UINT32_C(62500000),
            PM_TIMER_FREQUENCY_HZ,
            PM_TIMER_FREQUENCY_HZ,
            &rate
        ) != APIC_TIMER_STATUS_OK ||
        rate != UINT64_C(62500000)) {
        return false;
    }

    /*
     * Twice the reference span means the same count implies half the rate. Two
     * seconds is used rather than half a second because the reference rate is
     * odd, so halving it would truncate and the expectation would stop being
     * exact; doubling it cannot.
     */
    if (rate_from_countdown(
            UINT32_C(0xFFFFFFFF),
            UINT32_C(0xFFFFFFFF) - UINT32_C(62500000),
            PM_TIMER_FREQUENCY_HZ,
            (uint64_t)PM_TIMER_FREQUENCY_HZ * 2U,
            &rate
        ) != APIC_TIMER_STATUS_OK ||
        rate != UINT64_C(31250000)) {
        return false;
    }

    /* A counter that never moved cannot be timed. */
    if (rate_from_countdown(
            UINT32_C(0xFFFFFFFF),
            UINT32_C(0xFFFFFFFF),
            PM_TIMER_FREQUENCY_HZ,
            PM_TIMER_FREQUENCY_HZ,
            &rate
        ) != APIC_TIMER_STATUS_NOT_COUNTING) {
        return false;
    }

    /* A counter that ran backwards is not a countdown. */
    if (rate_from_countdown(
            UINT32_C(1000),
            UINT32_C(2000),
            PM_TIMER_FREQUENCY_HZ,
            PM_TIMER_FREQUENCY_HZ,
            &rate
        ) != APIC_TIMER_STATUS_NOT_COUNTING) {
        return false;
    }

    /* Reaching zero makes the elapsed count a floor, not a measurement. */
    if (rate_from_countdown(
            UINT32_C(0xFFFFFFFF),
            0U,
            PM_TIMER_FREQUENCY_HZ,
            PM_TIMER_FREQUENCY_HZ,
            &rate
        ) != APIC_TIMER_STATUS_COUNTER_EXHAUSTED) {
        return false;
    }

    /*
     * A reference that reports no rate, and one that reports no elapsed ticks.
     * The second is why calibration scales by the observed span: a wait that
     * returned without the counter moving must not divide by zero.
     */
    if (rate_from_countdown(
            UINT32_C(0xFFFFFFFF),
            UINT32_C(0xFFFFFFF0),
            0U,
            PM_TIMER_FREQUENCY_HZ,
            &rate
        ) != APIC_TIMER_STATUS_BAD_FREQUENCY ||
        rate_from_countdown(
            UINT32_C(0xFFFFFFFF),
            UINT32_C(0xFFFFFFF0),
            PM_TIMER_FREQUENCY_HZ,
            0U,
            &rate
        ) != APIC_TIMER_STATUS_BAD_FREQUENCY) {
        return false;
    }

    if (count_for_frequency(UINT64_C(62500000), UINT32_C(100), &count) !=
            APIC_TIMER_STATUS_OK ||
        count != UINT32_C(625000)) {
        return false;
    }

    /* A frequency faster than the counter can express is refused. */
    if (count_for_frequency(UINT64_C(62500000), UINT32_C(100000000), &count) !=
        APIC_TIMER_STATUS_BAD_FREQUENCY) {
        return false;
    }

    /* So is one so slow the count would not fit in the register. */
    if (count_for_frequency(UINT64_C(0x1000000000), UINT32_C(1), &count) !=
        APIC_TIMER_STATUS_BAD_FREQUENCY) {
        return false;
    }

    return count_for_frequency(UINT64_C(62500000), 0U, &count) ==
            APIC_TIMER_STATUS_BAD_FREQUENCY &&
        apic_timer_wait_for_ticks(1U) == APIC_TIMER_STATUS_NOT_RUNNING &&
        apic_timer_stop() == APIC_TIMER_STATUS_NOT_RUNNING;
}

const char *apic_timer_status_string(enum apic_timer_status status)
{
    switch (status) {
    case APIC_TIMER_STATUS_OK:
        return "ok";
    case APIC_TIMER_STATUS_APIC_OFFLINE:
        return "local APIC timer requires an online local APIC";
    case APIC_TIMER_STATUS_INTERRUPTS_ENABLED:
        return "local APIC timer mutation requires interrupts disabled";
    case APIC_TIMER_STATUS_ALREADY_CALIBRATED:
        return "local APIC timer was calibrated twice";
    case APIC_TIMER_STATUS_NOT_CALIBRATED:
        return "local APIC timer has no calibrated rate";
    case APIC_TIMER_STATUS_ALREADY_RUNNING:
        return "local APIC timer was started twice";
    case APIC_TIMER_STATUS_NOT_RUNNING:
        return "local APIC timer is not running";
    case APIC_TIMER_STATUS_REFERENCE_FAILURE:
        return "local APIC timer reference clock failed";
    case APIC_TIMER_STATUS_NOT_COUNTING:
        return "local APIC timer did not count down";
    case APIC_TIMER_STATUS_COUNTER_EXHAUSTED:
        return "local APIC timer reached zero during calibration";
    case APIC_TIMER_STATUS_BAD_FREQUENCY:
        return "local APIC timer frequency cannot be represented";
    case APIC_TIMER_STATUS_INTERRUPT_FAILURE:
        return "local APIC timer handler operation failed";
    case APIC_TIMER_STATUS_READBACK_MISMATCH:
        return "local APIC timer did not read back its programmed state";
    default:
        return "unknown local APIC timer status";
    }
}
