/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_MATH_H
#define SAPOTE_MATH_H

#define HUGE_VAL (__builtin_huge_val())
#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))
#define isfinite(value) __builtin_isfinite(value)
#define isinf(value) __builtin_isinf(value)
#define isnan(value) __builtin_isnan(value)

double floor(double value);
double ceil(double value);
double trunc(double value);
double fabs(double value);
double fmod(double left, double right);
double pow(double base, double exponent);
double frexp(double value, int *exponent);
double ldexp(double value, int exponent);
double sqrt(double value);

#endif
