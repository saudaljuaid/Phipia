/* SPDX-License-Identifier: GPL-3.0-only */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "../src/kernel/heap_core.h"

static struct heap_core_state state;

int main(void)
{
    char operation;

    while (scanf(" %c", &operation) == 1) {
        uint64_t first;
        uint64_t second;
        uint64_t result = UINT64_MAX;
        enum heap_core_status status;

        switch (operation) {
        case 'I':
            if (scanf(" %" SCNu64, &first) != 1) {
                return 2;
            }

            heap_core_clear(&state);
            status = heap_core_initialize(&state, first);
            printf("I %u\n", (unsigned int)status);
            break;
        case 'G':
            if (scanf(" %" SCNu64, &first) != 1) {
                return 2;
            }

            status = heap_core_grow(&state, first);
            printf("G %u\n", (unsigned int)status);
            break;
        case 'N':
            if (scanf(" %" SCNu64 " %" SCNu64, &first, &second) != 2) {
                return 2;
            }

            status = heap_core_growth_needed(&state, first, second, &result);
            printf("N %u %" PRIu64 "\n", (unsigned int)status, result);
            break;
        case 'A':
            if (scanf(" %" SCNu64 " %" SCNu64, &first, &second) != 2) {
                return 2;
            }

            status = heap_core_allocate(&state, first, second, &result);
            printf("A %u %" PRIu64 "\n", (unsigned int)status, result);
            break;
        case 'F':
            if (scanf(" %" SCNu64, &first) != 1) {
                return 2;
            }

            status = heap_core_deallocate(&state, first);
            printf("F %u\n", (unsigned int)status);
            break;
        case 'S': {
            struct heap_core_stats stats = {0};

            status = heap_core_get_stats(&state, &stats);
            printf(
                "S %u %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
                " %" PRIu64 " %" PRIu64 " %" PRIu64 " %u %u\n",
                (unsigned int)status,
                stats.capacity,
                stats.committed,
                stats.live_allocations,
                stats.live_requested_bytes,
                stats.live_extent_bytes,
                stats.reusable_bytes,
                stats.largest_reusable_block,
                (unsigned int)stats.descriptors_in_use,
                (unsigned int)stats.descriptor_limit
            );
            break;
        }
        case 'X':
            if (scanf(" %" SCNu64, &first) != 1 ||
                first >= HEAP_DESCRIPTOR_LIMIT) {
                return 2;
            }

            state.descriptors[(size_t)first].integrity ^= UINT64_C(1);
            status = heap_core_validate(&state);
            printf("X %u\n", (unsigned int)status);
            break;
        default:
            return 2;
        }
    }

    return ferror(stdin) != 0 ? 3 : 0;
}
