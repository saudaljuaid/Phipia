/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_LINUX_SIMD_H
#define SAPOTE_LINUX_SIMD_H

#include <stdbool.h>
#include <stddef.h>

#define LINUX_SIMD_FOUNDATION_CONTROLS 13U

bool linux_simd_foundation_install(size_t *completed_tests);
bool linux_simd_foundation_active(void);
bool linux_simd_reset_user(void);

#endif
