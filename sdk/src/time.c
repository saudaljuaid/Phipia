/* SPDX-License-Identifier: GPL-3.0-only */
#include <time.h>

#include <errno.h>
#include <stddef.h>

#include <sapote/runtime.h>

time_t time(time_t *output)
{
    const time_t value = (time_t)(sapote_monotonic_ns() / UINT64_C(1000000000));
    if (output != NULL) *output = value;
    return value;
}
clock_t clock(void) { return (clock_t)(sapote_monotonic_ns() / 1000U); }
int clock_gettime(int identifier, struct timespec *result)
{
    uint64_t now;
    if (identifier != CLOCK_MONOTONIC || result == NULL) { errno = EINVAL; return -1; }
    now = sapote_monotonic_ns();
    result->tv_sec = (time_t)(now / UINT64_C(1000000000));
    result->tv_nsec = (long)(now % UINT64_C(1000000000));
    return 0;
}
int nanosleep(const struct timespec *request, struct timespec *remaining)
{
    uint64_t interval;
    long result;
    if (request == NULL || request->tv_sec < 0 || request->tv_nsec < 0 ||
        request->tv_nsec >= 1000000000L ||
        (uint64_t)request->tv_sec > UINT64_MAX / UINT64_C(1000000000)) {
        errno = EINVAL; return -1;
    }
    interval = (uint64_t)request->tv_sec * UINT64_C(1000000000) +
        (uint64_t)request->tv_nsec;
    result = sapote_sleep_until(sapote_monotonic_ns() + interval);
    if (remaining != NULL) { remaining->tv_sec = 0; remaining->tv_nsec = 0; }
    return sapote_result(result);
}
double difftime(time_t end, time_t beginning) { return (double)(end - beginning); }
struct tm *gmtime(const time_t *value) { (void)value; errno = ENOTSUP; return NULL; }
struct tm *localtime(const time_t *value) { return gmtime(value); }
time_t mktime(struct tm *value) { (void)value; errno = ENOTSUP; return (time_t)-1; }
size_t strftime(char *output, size_t capacity, const char *format, const struct tm *value)
{
    (void)output; (void)capacity; (void)format; (void)value; errno = ENOTSUP; return 0U;
}
