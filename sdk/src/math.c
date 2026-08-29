/* SPDX-License-Identifier: GPL-3.0-only */
#include <math.h>

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

double fabs(double value) { return value < 0.0 ? -value : value; }
double trunc(double value)
{
    if (!isfinite(value) || value > (double)LLONG_MAX || value < (double)LLONG_MIN) return value;
    return (double)(long long)value;
}
double floor(double value)
{
    const double result = trunc(value);
    return result > value ? result - 1.0 : result;
}
double ceil(double value)
{
    const double result = trunc(value);
    return result < value ? result + 1.0 : result;
}
double fmod(double left, double right)
{
    return right == 0.0 ? NAN : left - trunc(left / right) * right;
}
double pow(double base, double exponent)
{
    long long power;
    double result = 1.0;
    int negative;
    if (!isfinite(base) || !isfinite(exponent)) return NAN;
    power = (long long)exponent;
    if ((double)power != exponent) return NAN;
    negative = power < 0;
    if (negative) power = -power;
    while (power != 0) {
        if ((power & 1) != 0) result *= base;
        base *= base;
        power >>= 1;
    }
    return negative ? 1.0 / result : result;
}
double frexp(double value, int *exponent)
{
    int result = 0;
    double magnitude = fabs(value);
    if (exponent == NULL || value == 0.0 || !isfinite(value)) {
        if (exponent != NULL) *exponent = 0;
        return value;
    }
    while (magnitude >= 1.0) { magnitude *= 0.5; ++result; }
    while (magnitude < 0.5) { magnitude *= 2.0; --result; }
    *exponent = result;
    return value < 0.0 ? -magnitude : magnitude;
}
double ldexp(double value, int exponent)
{
    while (exponent > 0) { value *= 2.0; --exponent; }
    while (exponent < 0) { value *= 0.5; ++exponent; }
    return value;
}
double sqrt(double value)
{
    double estimate;
    if (value < 0.0) return NAN;
    if (value == 0.0 || !isfinite(value)) return value;
    estimate = value > 1.0 ? value : 1.0;
    for (int iteration = 0; iteration < 32; ++iteration) {
        estimate = (estimate + value / estimate) * 0.5;
    }
    return estimate;
}
