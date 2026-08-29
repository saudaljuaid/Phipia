/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdlib.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

static unsigned long long parse_integer(
    const char *text,
    char **end,
    int base,
    int *negative
)
{
    const char *cursor = text;
    unsigned long long result = 0U;
    int any = 0;

    while (isspace((unsigned char)*cursor)) ++cursor;
    *negative = 0;
    if (*cursor == '+' || *cursor == '-') {
        *negative = *cursor == '-';
        ++cursor;
    }
    if ((base == 0 || base == 16) && cursor[0] == '0' &&
        (cursor[1] == 'x' || cursor[1] == 'X')) {
        base = 16;
        cursor += 2;
    } else if (base == 0) {
        base = *cursor == '0' ? 8 : 10;
    }
    if (base < 2 || base > 36) {
        errno = EINVAL;
        if (end != NULL) *end = (char *)(uintptr_t)text;
        return 0U;
    }
    for (;;) {
        int digit;
        if (*cursor >= '0' && *cursor <= '9') digit = *cursor - '0';
        else if (*cursor >= 'a' && *cursor <= 'z') digit = *cursor - 'a' + 10;
        else if (*cursor >= 'A' && *cursor <= 'Z') digit = *cursor - 'A' + 10;
        else break;
        if (digit >= base) break;
        any = 1;
        if (result > (ULLONG_MAX - (unsigned int)digit) /
                (unsigned int)base) {
            result = ULLONG_MAX;
            errno = ERANGE;
        } else {
            result = result * (unsigned int)base + (unsigned int)digit;
        }
        ++cursor;
    }
    if (end != NULL) *end = (char *)(uintptr_t)(any ? cursor : text);
    return result;
}

unsigned long long strtoull(const char *text, char **end, int base)
{
    int negative;
    unsigned long long result = parse_integer(text, end, base, &negative);
    return negative ? 0U - result : result;
}
long long strtoll(const char *text, char **end, int base)
{
    int negative;
    const unsigned long long value = parse_integer(text, end, base, &negative);
    const unsigned long long maximum = negative ?
        (unsigned long long)LLONG_MAX + 1U : (unsigned long long)LLONG_MAX;
    if (value > maximum) { errno = ERANGE; return negative ? LLONG_MIN : LLONG_MAX; }
    if (negative && value == (unsigned long long)LLONG_MAX + 1U) {
        return LLONG_MIN;
    }
    return negative ? -(long long)value : (long long)value;
}
unsigned long strtoul(const char *text, char **end, int base)
{
    const unsigned long long value = strtoull(text, end, base);
    if (value > ULONG_MAX) { errno = ERANGE; return ULONG_MAX; }
    return (unsigned long)value;
}
long strtol(const char *text, char **end, int base)
{
    const long long value = strtoll(text, end, base);
    if (value > LONG_MAX) { errno = ERANGE; return LONG_MAX; }
    if (value < LONG_MIN) { errno = ERANGE; return LONG_MIN; }
    return (long)value;
}

double strtod(const char *text, char **end)
{
    const char *cursor = text;
    double result = 0.0;
    double scale = 1.0;
    int negative = 0;
    int exponent = 0;
    int exponent_negative = 0;
    int any = 0;

    while (isspace((unsigned char)*cursor)) ++cursor;
    if (*cursor == '+' || *cursor == '-') { negative = *cursor == '-'; ++cursor; }
    while (isdigit((unsigned char)*cursor)) {
        result = result * 10.0 + (*cursor++ - '0'); any = 1;
    }
    if (*cursor == '.') {
        ++cursor;
        while (isdigit((unsigned char)*cursor)) {
            result = result * 10.0 + (*cursor++ - '0');
            scale *= 10.0; any = 1;
        }
    }
    result /= scale;
    if (any && (*cursor == 'e' || *cursor == 'E')) {
        const char *saved = cursor++;
        if (*cursor == '+' || *cursor == '-') { exponent_negative = *cursor == '-'; ++cursor; }
        if (!isdigit((unsigned char)*cursor)) cursor = saved;
        else while (isdigit((unsigned char)*cursor)) {
            if (exponent < 10000) exponent = exponent * 10 + (*cursor - '0');
            ++cursor;
        }
    }
    while (exponent-- > 0) result = exponent_negative ? result / 10.0 : result * 10.0;
    if (end != NULL) *end = (char *)(uintptr_t)(any ? cursor : text);
    return negative ? -result : result;
}

int atoi(const char *text) { return (int)strtol(text, NULL, 10); }
int abs(int value) { return value < 0 ? -value : value; }
long labs(long value) { return value < 0 ? -value : value; }

static void swap_bytes(unsigned char *left, unsigned char *right, size_t size)
{
    while (size-- != 0U) { const unsigned char value = *left; *left++ = *right; *right++ = value; }
}

void qsort(void *base, size_t count, size_t size,
    int (*compare)(const void *, const void *))
{
    unsigned char *bytes = base;
    if (bytes == NULL || compare == NULL || size == 0U) return;
    for (size_t index = 1U; index < count; ++index) {
        size_t position = index;
        while (position != 0U && compare(bytes + (position - 1U) * size,
                bytes + position * size) > 0) {
            swap_bytes(bytes + (position - 1U) * size,
                bytes + position * size, size);
            --position;
        }
    }
}

void *bsearch(const void *key, const void *base, size_t count, size_t size,
    int (*compare)(const void *, const void *))
{
    const unsigned char *bytes = base;
    size_t low = 0U, high = count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2U;
        const int order = compare(key, bytes + middle * size);
        if (order == 0) return (void *)(uintptr_t)(bytes + middle * size);
        if (order < 0) high = middle; else low = middle + 1U;
    }
    return NULL;
}

static unsigned int random_state = 1U;
void srand(unsigned int seed) { random_state = seed; }
int rand(void)
{
    random_state = random_state * 1103515245U + 12345U;
    return (int)((random_state >> 1U) & RAND_MAX);
}

int system(const char *command)
{
    if (command == NULL) return 0;
    errno = ENOTSUP;
    return -1;
}
