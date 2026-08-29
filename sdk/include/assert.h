/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_ASSERT_H
#define SAPOTE_ASSERT_H

void __sapote_assert(const char *expression, const char *file, int line);
#define assert(expression) ((expression) ? (void)0 : \
    __sapote_assert(#expression, __FILE__, __LINE__))

#endif
