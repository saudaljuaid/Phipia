/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "syscall_core.h"

static void decoded_clear(struct syscall_decoded *decoded)
{
    decoded->action = SYSCALL_ACTION_NONE;
    decoded->exit_status = 0;
    decoded->descriptor = 0U;
    decoded->event_index = 0U;
    decoded->user_address = 0U;
    decoded->length = 0U;
    decoded->event_epoch = 0U;
    decoded->process = 0U;
    decoded->wake_all = 0U;
    for (size_t index = 0U; index < sizeof(decoded->reserved); ++index) {
        decoded->reserved[index] = 0U;
    }
}

static bool arguments_zero_from(
    const struct syscall_request *request,
    size_t first
)
{
    for (size_t index = first; index < SYSCALL_ARGUMENT_COUNT; ++index) {
        if (request->arguments[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool process_handle_syntax_valid(process_handle_t process)
{
    const uint64_t encoded_slot = process & UINT64_C(0xFFFFFFFF);

    return encoded_slot >= 1U && encoded_slot <= PROCESS_LIMIT &&
        (process >> 32U) != 0U;
}

static bool user_range_syntax_valid(uint64_t address, uint64_t length)
{
    if (length == 0U) {
        return true;
    }
    return address >= PROCESS_USER_ADDRESS_MIN &&
        address <= PROCESS_USER_ADDRESS_MAX &&
        length - 1U <= PROCESS_USER_ADDRESS_MAX - address;
}

static bool decode_exit_status(uint64_t encoded, int32_t *status)
{
    const uint32_t low = (uint32_t)encoded;
    const uint32_t high = (uint32_t)(encoded >> 32U);
    int64_t signed_value;

    if (high != 0U &&
        (high != UINT32_MAX || (low & UINT32_C(0x80000000)) == 0U)) {
        return false;
    }
    signed_value = (int64_t)low -
        (int64_t)(low >> 31U) * INT64_C(4294967296);
    *status = (int32_t)signed_value;
    return true;
}

enum syscall_status syscall_core_decode(
    const struct syscall_request *request,
    struct syscall_decoded *decoded
)
{
    if (request == NULL || decoded == NULL) {
        return SYSCALL_STATUS_NULL_ARGUMENT;
    }
    decoded_clear(decoded);
    if (request->number >= SYSCALL_NUMBER_LIMIT) {
        return SYSCALL_STATUS_UNKNOWN_NUMBER;
    }

    switch ((enum syscall_number)request->number) {
    case SYSCALL_NUMBER_EXIT:
        if (!arguments_zero_from(request, 1U)) {
            return SYSCALL_STATUS_RESERVED_ARGUMENT;
        }
        if (!decode_exit_status(
                request->arguments[0],
                &decoded->exit_status
            )) {
            return SYSCALL_STATUS_INVALID_EXIT_STATUS;
        }
        decoded->action = SYSCALL_ACTION_EXIT;
        return SYSCALL_STATUS_OK;
    case SYSCALL_NUMBER_YIELD:
        if (!arguments_zero_from(request, 0U)) {
            return SYSCALL_STATUS_RESERVED_ARGUMENT;
        }
        decoded->action = SYSCALL_ACTION_YIELD;
        return SYSCALL_STATUS_OK;
    case SYSCALL_NUMBER_GETPID:
        if (!arguments_zero_from(request, 0U)) {
            return SYSCALL_STATUS_RESERVED_ARGUMENT;
        }
        decoded->action = SYSCALL_ACTION_GETPID;
        return SYSCALL_STATUS_OK;
    case SYSCALL_NUMBER_WRITE:
        if (!arguments_zero_from(request, 3U)) {
            return SYSCALL_STATUS_RESERVED_ARGUMENT;
        }
        if (request->arguments[0] != 1U &&
            request->arguments[0] != 2U) {
            return SYSCALL_STATUS_INVALID_DESCRIPTOR;
        }
        if (request->arguments[2] > SYSCALL_WRITE_LIMIT) {
            return SYSCALL_STATUS_WRITE_LIMIT;
        }
        if (!user_range_syntax_valid(
                request->arguments[1],
                request->arguments[2]
            )) {
            return SYSCALL_STATUS_INVALID_USER_RANGE;
        }
        decoded->action = SYSCALL_ACTION_WRITE;
        decoded->descriptor = (uint32_t)request->arguments[0];
        decoded->user_address = request->arguments[1];
        decoded->length = request->arguments[2];
        return SYSCALL_STATUS_OK;
    case SYSCALL_NUMBER_EVENT_WAIT:
        if (!arguments_zero_from(request, 2U)) {
            return SYSCALL_STATUS_RESERVED_ARGUMENT;
        }
        if (request->arguments[0] >= SYSCALL_EVENT_LIMIT) {
            return SYSCALL_STATUS_INVALID_EVENT;
        }
        if (request->arguments[1] == 0U) {
            return SYSCALL_STATUS_INVALID_EPOCH;
        }
        decoded->action = SYSCALL_ACTION_EVENT_WAIT;
        decoded->event_index = (uint32_t)request->arguments[0];
        decoded->event_epoch = request->arguments[1];
        return SYSCALL_STATUS_OK;
    case SYSCALL_NUMBER_EVENT_WAKE:
        if (!arguments_zero_from(request, 2U)) {
            return SYSCALL_STATUS_RESERVED_ARGUMENT;
        }
        if (request->arguments[0] >= SYSCALL_EVENT_LIMIT) {
            return SYSCALL_STATUS_INVALID_EVENT;
        }
        if (request->arguments[1] > 1U) {
            return SYSCALL_STATUS_INVALID_WAKE_MODE;
        }
        decoded->action = SYSCALL_ACTION_EVENT_WAKE;
        decoded->event_index = (uint32_t)request->arguments[0];
        decoded->wake_all = (uint8_t)request->arguments[1];
        return SYSCALL_STATUS_OK;
    case SYSCALL_NUMBER_PROCESS_WAIT:
        if (!arguments_zero_from(request, 1U)) {
            return SYSCALL_STATUS_RESERVED_ARGUMENT;
        }
        if (!process_handle_syntax_valid(request->arguments[0])) {
            return SYSCALL_STATUS_INVALID_PROCESS;
        }
        decoded->action = SYSCALL_ACTION_PROCESS_WAIT;
        decoded->process = request->arguments[0];
        return SYSCALL_STATUS_OK;
    case SYSCALL_NUMBER_LIMIT:
        break;
    }
    return SYSCALL_STATUS_UNKNOWN_NUMBER;
}
