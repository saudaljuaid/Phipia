/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_SYSCALL_CORE_H
#define ZENITH_SYSCALL_CORE_H

#include <zenith/syscall.h>

enum syscall_status syscall_core_decode(
    const struct syscall_request *request,
    struct syscall_decoded *decoded
);

#endif
