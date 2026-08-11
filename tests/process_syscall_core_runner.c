/* SPDX-License-Identifier: GPL-3.0-only */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../src/kernel/process_core.h"
#include "../src/kernel/syscall_core.h"
#include "../src/kernel/usercopy_core.h"

static void print_process_status(char operation, enum process_status status)
{
    printf("%c %d\n", operation, (int)status);
}

static bool self_checks(void)
{
    struct process_core_state core;
    process_handle_t root;
    process_handle_t child;
    process_handle_t stale_child;
    struct process_wait_token token;
    int32_t exit_status;
    struct process_user_frame frame = {
        .rip = UINT64_C(0x400000),
        .cs = PROCESS_USER_CODE_SELECTOR,
        .rflags = PROCESS_RFLAGS_RESERVED | PROCESS_RFLAGS_INTERRUPT,
        .rsp = UINT64_C(0x800000),
        .ss = PROCESS_USER_DATA_SELECTOR
    };
    struct syscall_request request = {0};
    struct syscall_decoded decoded;
    struct usercopy_validation validation;
    struct address_space_mapping_snapshot mappings[2] = {
        {
            .virtual_page = 1U,
            .physical_frame = 10U,
            .permissions = ADDRESS_SPACE_PERMISSION_READ |
                ADDRESS_SPACE_PERMISSION_USER,
            .user_accessible = true,
            .writable = false,
            .executable = false
        },
        {
            .virtual_page = 2U,
            .physical_frame = 11U,
            .permissions = ADDRESS_SPACE_PERMISSION_READ |
                ADDRESS_SPACE_PERMISSION_WRITE |
                ADDRESS_SPACE_PERMISSION_USER,
            .user_accessible = true,
            .writable = true,
            .executable = false
        }
    };

    process_core_clear(&core);
    if (!process_core_resolution_output_self_test() ||
        process_core_initialize(NULL) != PROCESS_STATUS_NULL_ARGUMENT ||
        process_core_validate(NULL) != PROCESS_STATUS_NULL_ARGUMENT ||
        process_core_initialize(&core) != PROCESS_STATUS_OK ||
        process_core_initialize(&core) !=
            PROCESS_STATUS_ALREADY_INITIALIZED ||
        process_core_validate(&core) != PROCESS_STATUS_OK ||
        process_core_validate_initial_frame(&frame) != PROCESS_FRAME_OK ||
        process_core_validate_return_frame(&frame) != PROCESS_FRAME_OK ||
        process_core_fault_disposition(PROCESS_USER_CODE_SELECTOR) !=
            PROCESS_FAULT_KILL_OFFENDING_PROCESS ||
        process_core_fault_disposition(UINT64_C(8)) !=
            PROCESS_FAULT_KERNEL_FATAL) {
        return false;
    }

    root = 0U;
    child = 0U;
    if (process_core_create(
            &core,
            0U,
            (UINT64_C(1) << 32U) | UINT64_C(1),
            &root
        ) != PROCESS_STATUS_OK || root == 0U ||
        process_core_attach_task(&core, root) != PROCESS_STATUS_OK ||
        process_core_block(&core, root) != PROCESS_STATUS_OK ||
        process_core_make_runnable(&core, root) != PROCESS_STATUS_OK ||
        process_core_create(
            &core,
            root,
            (UINT64_C(1) << 32U) | UINT64_C(2),
            &child
        ) != PROCESS_STATUS_OK || child == 0U ||
        process_core_wait_observe(
            &core,
            root,
            child,
            &token,
            &exit_status
        ) != PROCESS_STATUS_WOULD_BLOCK || token.process != child ||
        token.epoch != 1U || exit_status != 0 ||
        process_core_exit(&core, child, -7) != PROCESS_STATUS_OK ||
        process_core_wait_complete(&core, root, token, &exit_status) !=
            PROCESS_STATUS_OK || exit_status != -7 ||
        process_core_begin_reap(&core, child) != PROCESS_STATUS_OK) {
        return false;
    }
    stale_child = child;
    if (process_core_reap(&core, child) != PROCESS_STATUS_OK ||
        process_core_snapshot(&core, stale_child, NULL) !=
            PROCESS_STATUS_NULL_ARGUMENT ||
        process_core_attach_task(&core, stale_child) !=
            PROCESS_STATUS_STALE_HANDLE ||
        process_core_detach_task(&core, root) != PROCESS_STATUS_OK ||
        process_core_exit(&core, root, 0) != PROCESS_STATUS_OK ||
        process_core_begin_reap(&core, root) != PROCESS_STATUS_OK ||
        process_core_reap(&core, root) != PROCESS_STATUS_OK ||
        process_core_validate(&core) != PROCESS_STATUS_OK) {
        return false;
    }

    process_core_clear(&core);
    if (process_core_initialize(&core) != PROCESS_STATUS_OK) {
        return false;
    }
    for (size_t index = 0U; index < PROCESS_LIMIT; ++index) {
        core.slots[index].generation = UINT32_MAX;
    }
    if (process_core_create(
            &core,
            0U,
            (UINT64_C(1) << 32U) | UINT64_C(1),
            &root
        ) != PROCESS_STATUS_GENERATION_EXHAUSTED) {
        return false;
    }

    process_core_clear(&core);
    if (process_core_initialize(&core) != PROCESS_STATUS_OK ||
        process_core_create(
            &core,
            0U,
            (UINT64_C(1) << 32U) | UINT64_C(1),
            &root
        ) != PROCESS_STATUS_OK) {
        return false;
    }
    core.slots[0].task_attachment_count =
        PROCESS_TASK_ATTACHMENT_LIMIT;
    if (process_core_attach_task(&core, root) !=
        PROCESS_STATUS_TASK_LIMIT) {
        return false;
    }
    core.slots[0].task_attachment_count = 0U;
    core.slots[0].completion_epoch = UINT64_MAX;
    if (process_core_exit(&core, root, 1) !=
        PROCESS_STATUS_EVENT_EPOCH_EXHAUSTED) {
        return false;
    }

    frame.rflags = PROCESS_RFLAGS_RESERVED;
    if (process_core_validate_return_frame(&frame) !=
        PROCESS_FRAME_BAD_FLAGS) {
        return false;
    }
    frame.rflags = PROCESS_RFLAGS_USER_ALLOWED;
    if (process_core_validate_return_frame(&frame) != PROCESS_FRAME_OK) {
        return false;
    }
    frame.rflags = PROCESS_RFLAGS_RESERVED | PROCESS_RFLAGS_INTERRUPT;
    frame.cs = UINT64_C(8);
    if (process_core_validate_return_frame(&frame) !=
        PROCESS_FRAME_BAD_CODE_SELECTOR) {
        return false;
    }
    request.number = SYSCALL_NUMBER_YIELD;
    if (syscall_core_decode(NULL, &decoded) !=
            SYSCALL_STATUS_NULL_ARGUMENT ||
        syscall_core_decode(&request, NULL) !=
            SYSCALL_STATUS_NULL_ARGUMENT ||
        syscall_core_decode(&request, &decoded) != SYSCALL_STATUS_OK ||
        decoded.action != SYSCALL_ACTION_YIELD ||
        usercopy_core_validate(
            0U,
            0U,
            USERCOPY_ACCESS_READ_FROM_USER,
            NULL,
            0U,
            &validation
        ) != USERCOPY_STATUS_OK ||
        usercopy_core_validate(
            0U,
            1U,
            USERCOPY_ACCESS_READ_FROM_USER,
            NULL,
            0U,
            &validation
        ) != USERCOPY_STATUS_NULL_PAGE) {
        return false;
    }
    if (usercopy_core_validate(
            UINT64_C(4096),
            UINT64_C(4097),
            USERCOPY_ACCESS_READ_FROM_USER,
            mappings,
            2U,
            &validation
        ) != USERCOPY_STATUS_OK || validation.page_count != 2U ||
        !validation.crosses_page ||
        usercopy_core_validate(
            UINT64_C(4096),
            UINT64_C(4097),
            USERCOPY_ACCESS_WRITE_TO_USER,
            mappings,
            2U,
            &validation
        ) != USERCOPY_STATUS_WRITE_DENIED ||
        validation.failed_page != 1U) {
        return false;
    }
    if (usercopy_core_validate(
            UINT64_C(4097),
            USERCOPY_LENGTH_LIMIT,
            USERCOPY_ACCESS_READ_FROM_USER,
            NULL,
            0U,
            &validation
        ) != USERCOPY_STATUS_CROSS_PAGE_LIMIT ||
        validation.first_page != 1U || validation.last_page != 17U ||
        validation.page_count != 0U || !validation.crosses_page) {
        return false;
    }
    core.slots[0].reserved[0] = UINT8_C(1);
    return process_core_validate(&core) == PROCESS_STATUS_CORRUPTED;
}

int main(void)
{
    struct process_core_state core;
    char operation;

    process_core_clear(&core);
    while (scanf(" %c", &operation) == 1) {
        process_handle_t handle;
        enum process_status status;

        switch (operation) {
        case 'Z':
            process_core_clear(&core);
            puts("Z");
            break;
        case 'I':
            print_process_status(operation, process_core_initialize(&core));
            break;
        case 'C': {
            process_handle_t parent;
            address_space_handle_t address_space;

            if (scanf(" %" SCNu64 " %" SCNu64,
                    &parent,
                    &address_space) != 2) {
                return 2;
            }
            handle = 0U;
            status = process_core_create(
                &core,
                parent,
                address_space,
                &handle
            );
            printf("C %d %" PRIu64 "\n", (int)status, handle);
            break;
        }
        case 'A':
        case 'D':
        case 'B':
        case 'R':
        case 'Y':
        case 'K':
            if (scanf(" %" SCNu64, &handle) != 1) {
                return 2;
            }
            if (operation == 'A') {
                status = process_core_attach_task(&core, handle);
            } else if (operation == 'D') {
                status = process_core_detach_task(&core, handle);
            } else if (operation == 'B') {
                status = process_core_block(&core, handle);
            } else if (operation == 'R') {
                status = process_core_make_runnable(&core, handle);
            } else if (operation == 'Y') {
                status = process_core_begin_reap(&core, handle);
            } else {
                status = process_core_reap(&core, handle);
            }
            print_process_status(operation, status);
            break;
        case 'E': {
            int32_t exit_status;

            if (scanf(" %" SCNu64 " %" SCNd32,
                    &handle,
                    &exit_status) != 2) {
                return 2;
            }
            print_process_status(
                operation,
                process_core_exit(&core, handle, exit_status)
            );
            break;
        }
        case 'O': {
            process_handle_t child;
            struct process_wait_token token;
            int32_t exit_status;

            if (scanf(" %" SCNu64 " %" SCNu64,
                    &handle,
                    &child) != 2) {
                return 2;
            }
            status = process_core_wait_observe(
                &core,
                handle,
                child,
                &token,
                &exit_status
            );
            printf(
                "O %d %" PRIu64 " %" PRIu64 " %" PRId32 "\n",
                (int)status,
                token.process,
                token.epoch,
                exit_status
            );
            break;
        }
        case 'W': {
            struct process_wait_token token;
            int32_t exit_status;

            if (scanf(" %" SCNu64 " %" SCNu64 " %" SCNu64,
                    &handle,
                    &token.process,
                    &token.epoch) != 3) {
                return 2;
            }
            status = process_core_wait_complete(
                &core,
                handle,
                token,
                &exit_status
            );
            printf("W %d %" PRId32 "\n", (int)status, exit_status);
            break;
        }
        case 'G': {
            struct process_snapshot snapshot;

            if (scanf(" %" SCNu64, &handle) != 1) {
                return 2;
            }
            status = process_core_snapshot(&core, handle, &snapshot);
            printf(
                "G %d %d %u %" PRIu64 " %" PRIu64
                " %" PRIu64 " %u %u %" PRId32 "\n",
                (int)status,
                (int)snapshot.state,
                snapshot.generation,
                snapshot.parent,
                snapshot.address_space,
                snapshot.completion_epoch,
                (unsigned int)snapshot.task_attachment_count,
                (unsigned int)snapshot.child_count,
                snapshot.exit_status
            );
            break;
        }
        case 'X':
            printf(
                "X %d %u\n",
                (int)process_core_validate(&core),
                (unsigned int)core.occupied_count
            );
            break;
        case 'S': {
            struct syscall_request request;
            struct syscall_decoded decoded;
            enum syscall_status syscall_status;

            if (scanf(
                    " %" SCNu64 " %" SCNu64 " %" SCNu64
                    " %" SCNu64 " %" SCNu64 " %" SCNu64
                    " %" SCNu64,
                    &request.number,
                    &request.arguments[0],
                    &request.arguments[1],
                    &request.arguments[2],
                    &request.arguments[3],
                    &request.arguments[4],
                    &request.arguments[5]
                ) != 7) {
                return 2;
            }
            syscall_status = syscall_core_decode(&request, &decoded);
            printf(
                "S %d %d %" PRId32 " %u %u %" PRIu64
                " %" PRIu64 " %" PRIu64 " %" PRIu64 " %u\n",
                (int)syscall_status,
                (int)decoded.action,
                decoded.exit_status,
                decoded.descriptor,
                decoded.event_index,
                decoded.user_address,
                decoded.length,
                decoded.event_epoch,
                decoded.process,
                (unsigned int)decoded.wake_all
            );
            break;
        }
        case 'U': {
            struct address_space_mapping_snapshot mappings[33];
            struct usercopy_validation validation;
            enum usercopy_status usercopy_status;
            uint64_t address;
            uint64_t length;
            unsigned int access;
            size_t count;

            if (scanf(" %" SCNu64 " %" SCNu64 " %u %zu",
                    &address,
                    &length,
                    &access,
                    &count) != 4 || count > 33U) {
                return 2;
            }
            for (size_t index = 0U; index < count; ++index) {
                unsigned int permissions;
                unsigned int user;
                unsigned int writable;
                unsigned int executable;

                if (scanf(" %" SCNu64 " %u %u %u %u",
                        &mappings[index].virtual_page,
                        &permissions,
                        &user,
                        &writable,
                        &executable) != 5) {
                    return 2;
                }
                mappings[index].physical_frame = 0U;
                mappings[index].permissions = permissions;
                mappings[index].user_accessible = user != 0U;
                mappings[index].writable = writable != 0U;
                mappings[index].executable = executable != 0U;
            }
            usercopy_status = usercopy_core_validate(
                address,
                length,
                (enum usercopy_access)access,
                mappings,
                count,
                &validation
            );
            printf(
                "U %d %" PRIu64 " %" PRIu64 " %" PRIu64
                " %u %u\n",
                (int)usercopy_status,
                validation.first_page,
                validation.last_page,
                validation.failed_page,
                validation.page_count,
                validation.crosses_page ? 1U : 0U
            );
            break;
        }
        case 'J': {
            struct process_user_frame frame;
            unsigned int kind;
            enum process_frame_status frame_status;

            if (scanf(
                    " %u %" SCNu64 " %" SCNu64 " %" SCNu64
                    " %" SCNu64 " %" SCNu64,
                    &kind,
                    &frame.rip,
                    &frame.cs,
                    &frame.rflags,
                    &frame.rsp,
                    &frame.ss
                ) != 6) {
                return 2;
            }
            frame_status = kind == 0U ?
                process_core_validate_initial_frame(&frame) :
                process_core_validate_return_frame(&frame);
            printf("J %d\n", (int)frame_status);
            break;
        }
        case 'F': {
            uint64_t cs;

            if (scanf(" %" SCNu64, &cs) != 1) {
                return 2;
            }
            printf("F %d\n", (int)process_core_fault_disposition(cs));
            break;
        }
        case 'H':
            printf("H %u\n", self_checks() ? 1U : 0U);
            break;
        default:
            return 3;
        }
    }
    return 0;
}
