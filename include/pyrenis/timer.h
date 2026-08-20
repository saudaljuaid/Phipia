/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PYRENIS_TIMER_H
#define PYRENIS_TIMER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Deadlines, on top of the monotonic clock. Everything before this could measure
 * an interval that had already happened or count periodic ticks; nothing could
 * ask to be woken at a point in time. A scheduler needs that before it needs
 * anything else.
 */

/*
 * How many pending deadlines timer_start asks the kernel heap for. This is no
 * longer an array bound: the table is one heap allocation made once at start and
 * released at stop, and the capacity lives in a variable. It stays a compile-time
 * default only because nothing yet has an opinion about how many deadlines it
 * needs; timer_capacity reports what was actually obtained.
 *
 * The table cannot be grown on demand, and that is deliberate rather than
 * unfinished. timer_arm is reachable from inside the timer interrupt, because a
 * callback is allowed to arm a fresh deadline, and the heap is not reentrant.
 * Allocating per arm would put a heap transaction inside an interrupt handler.
 */
#define TIMER_MAX_PENDING 32U

/*
 * Runs inside the timer interrupt with interrupts disabled. It must not block,
 * and it must not assume the deadline it was armed for is the current time - it
 * is told the deadline it was armed for and can read the clock for the rest.
 */
typedef void (*timer_callback_t)(uint64_t deadline_ns, void *context);

enum timer_status {
    TIMER_STATUS_OK = 0,
    TIMER_STATUS_NULL_ARGUMENT,
    TIMER_STATUS_NOT_STARTED,
    TIMER_STATUS_ALREADY_STARTED,
    TIMER_STATUS_NO_CLOCK,
    TIMER_STATUS_NO_CAPACITY,
    TIMER_STATUS_NO_MEMORY,
    TIMER_STATUS_BAD_INTERVAL,
    TIMER_STATUS_UNKNOWN_TIMER,
    TIMER_STATUS_HARDWARE_FAILURE,
    TIMER_STATUS_EXPIRED_LATE
};

enum timer_status timer_start(void);
enum timer_status timer_stop(void);
bool timer_is_started(void);

/*
 * Arm a callback for a monotonic deadline. A deadline already in the past is
 * refused rather than fired immediately, because a caller that computed one has
 * a bug the timer would otherwise hide. The identifier is never zero, so zero
 * can be held as "no timer" without a separate flag.
 */
enum timer_status timer_arm(
    uint64_t deadline_ns,
    timer_callback_t callback,
    void *context,
    uint64_t *identifier
);

enum timer_status timer_cancel(uint64_t identifier);

/*
 * Block until at least this many nanoseconds have passed on the monotonic clock.
 * A deadline that has not arrived within twice its own interval plus a grace
 * period returns a status, and so does a loop that finds nothing armed to wake
 * it. What remains unbounded is an armed deadline the hardware never delivers,
 * which halts exactly as every other wait loop in this kernel does.
 */
enum timer_status timer_sleep_ns(uint64_t nanoseconds);

size_t timer_pending_count(void);
size_t timer_capacity(void);
uint64_t timer_expiry_count(void);
bool timer_self_test(void);
const char *timer_status_string(enum timer_status status);

#endif
