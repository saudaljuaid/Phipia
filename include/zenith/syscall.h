/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_SYSCALL_H
#define ZENITH_SYSCALL_H

#include <stdint.h>

#include <zenith/process.h>

#define SYSCALL_ARGUMENT_COUNT UINT32_C(6)
#define SYSCALL_EVENT_LIMIT UINT32_C(8)
#define SYSCALL_WRITE_LIMIT UINT64_C(4096)

enum syscall_number {
    SYSCALL_NUMBER_EXIT = 0,
    SYSCALL_NUMBER_YIELD,
    SYSCALL_NUMBER_GETPID,
    SYSCALL_NUMBER_WRITE,
    SYSCALL_NUMBER_EVENT_WAIT,
    SYSCALL_NUMBER_EVENT_WAKE,
    SYSCALL_NUMBER_PROCESS_WAIT,
    SYSCALL_NUMBER_LIMIT
};

enum syscall_action {
    SYSCALL_ACTION_NONE = 0,
    SYSCALL_ACTION_EXIT,
    SYSCALL_ACTION_YIELD,
    SYSCALL_ACTION_GETPID,
    SYSCALL_ACTION_WRITE,
    SYSCALL_ACTION_EVENT_WAIT,
    SYSCALL_ACTION_EVENT_WAKE,
    SYSCALL_ACTION_PROCESS_WAIT
};

enum syscall_status {
    SYSCALL_STATUS_OK = 0,
    SYSCALL_STATUS_NULL_ARGUMENT,
    SYSCALL_STATUS_UNKNOWN_NUMBER,
    SYSCALL_STATUS_RESERVED_ARGUMENT,
    SYSCALL_STATUS_INVALID_EXIT_STATUS,
    SYSCALL_STATUS_INVALID_DESCRIPTOR,
    SYSCALL_STATUS_INVALID_USER_RANGE,
    SYSCALL_STATUS_WRITE_LIMIT,
    SYSCALL_STATUS_INVALID_EVENT,
    SYSCALL_STATUS_INVALID_EPOCH,
    SYSCALL_STATUS_INVALID_WAKE_MODE,
    SYSCALL_STATUS_INVALID_PROCESS
};

struct syscall_request {
    uint64_t number;
    uint64_t arguments[SYSCALL_ARGUMENT_COUNT];
};

struct syscall_decoded {
    enum syscall_action action;
    int32_t exit_status;
    uint32_t descriptor;
    uint32_t event_index;
    uint64_t user_address;
    uint64_t length;
    uint64_t event_epoch;
    process_handle_t process;
    uint8_t wake_all;
    uint8_t reserved[7];
};

#endif
