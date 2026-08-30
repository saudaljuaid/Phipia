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

double sin(double value)
{
    const double pi = 3.14159265358979323846;
    const double two_pi = 6.28318530717958647692;
    double term;
    double result;

    if (!isfinite(value)) return NAN;
    value = fmod(value, two_pi);
    if (value > pi) value -= two_pi;
    if (value < -pi) value += two_pi;
    term = value;
    result = value;
    for (int index = 1; index <= 11; ++index) {
        const double divisor = (double)(2 * index) * (double)(2 * index + 1);
        term *= -value * value / divisor;
        result += term;
    }
    return result;
}

double cos(double value)
{
    const double pi = 3.14159265358979323846;
    const double two_pi = 6.28318530717958647692;
    double term = 1.0;
    double result = 1.0;

    if (!isfinite(value)) return NAN;
    value = fmod(value, two_pi);
    if (value > pi) value -= two_pi;
    if (value < -pi) value += two_pi;
    for (int index = 1; index <= 12; ++index) {
        const double divisor = (double)(2 * index - 1) * (double)(2 * index);
        term *= -value * value / divisor;
        result += term;
    }
    return result;
}

double tan(double value)
{
    return sin(value) / cos(value);
}

double atan(double value)
{
    const double half_pi = 1.57079632679489661923;
    const double quarter_pi = 0.78539816339744830962;
    double sign = 1.0;
    double offset = 0.0;
    double term;
    double result;

    if (isnan(value)) return NAN;
    if (value < 0.0) { sign = -1.0; value = -value; }
    if (isinf(value)) return sign * half_pi;
    if (value > 1.0) {
        value = 1.0 / value;
        offset = half_pi;
        sign = -sign;
    } else if (value > 0.41421356237309504880) {
        value = (value - 1.0) / (value + 1.0);
        offset = quarter_pi;
    }
    term = value;
    result = value;
    for (int index = 1; index <= 32; ++index) {
        term *= -value * value;
        result += term / (double)(2 * index + 1);
    }
    return offset + sign * result;
}

double atan2(double y, double x)
{
    const double pi = 3.14159265358979323846;
    const double half_pi = 1.57079632679489661923;

    if (isnan(x) || isnan(y)) return NAN;
    if (x > 0.0) return atan(y / x);
    if (x < 0.0) return atan(y / x) + (y < 0.0 ? -pi : pi);
    if (y > 0.0) return half_pi;
    if (y < 0.0) return -half_pi;
    return 0.0;
}

double asin(double value)
{
    if (value < -1.0 || value > 1.0) return NAN;
    return atan2(value, sqrt((1.0 - value) * (1.0 + value)));
}

double acos(double value)
{
    const double half_pi = 1.57079632679489661923;
    return half_pi - asin(value);
}

double log(double value)
{
    const double ln_two = 0.69314718055994530942;
    int exponent;
    double mantissa;
    double ratio;
    double square;
    double term;
    double result;

    if (value < 0.0 || isnan(value)) return NAN;
    if (value == 0.0) return -INFINITY;
    if (isinf(value)) return INFINITY;
    mantissa = frexp(value, &exponent);
    if (mantissa < 0.70710678118654752440) {
        mantissa *= 2.0;
        --exponent;
    }
    ratio = (mantissa - 1.0) / (mantissa + 1.0);
    square = ratio * ratio;
    term = ratio;
    result = ratio;
    for (int index = 1; index <= 24; ++index) {
        term *= square;
        result += term / (double)(2 * index + 1);
    }
    return 2.0 * result + (double)exponent * ln_two;
}

double log10(double value)
{
    return log(value) / 2.30258509299404568402;
}

double exp(double value)
{
    const double ln_two = 0.69314718055994530942;
    long exponent;
    double reduced;
    double term = 1.0;
    double result = 1.0;

    if (isnan(value)) return NAN;
    if (value > 709.0) return INFINITY;
    if (value < -745.0) return 0.0;
    exponent = (long)floor(value / ln_two + 0.5);
    reduced = value - (double)exponent * ln_two;
    for (int index = 1; index <= 24; ++index) {
        term *= reduced / (double)index;
        result += term;
    }
    return ldexp(result, (int)exponent);
}

double pow(double base, double exponent)
{
    if (isnan(base) || isnan(exponent)) return NAN;
    if (exponent == 0.0) return 1.0;
    if (base == 0.0) return exponent > 0.0 ? 0.0 : INFINITY;
    if (base < 0.0) {
        if (exponent >= (double)LLONG_MAX || exponent < (double)LLONG_MIN) {
            return exp(exponent * log(-base));
        }
        const long long integral = (long long)exponent;
        if ((double)integral != exponent) return NAN;
        return (integral & 1LL) != 0 ? -exp(exponent * log(-base)) :
            exp(exponent * log(-base));
    }
    return exp(exponent * log(base));
}
