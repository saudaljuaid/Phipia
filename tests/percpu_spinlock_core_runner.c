/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "../src/kernel/percpu_core.h"
#include "../src/kernel/spinlock_core.h"

static bool initialize_pair(
    struct percpu_core_state *cpus,
    struct spinlock_core_state *locks
)
{
    percpu_core_clear(cpus);
    spinlock_core_clear(locks);
    return percpu_core_initialize(cpus, 0U, 0U) == PERCPU_STATUS_OK &&
        spinlock_core_initialize(locks, cpus) == SPINLOCK_STATUS_OK;
}

static bool self_test(void)
{
    struct percpu_core_state cpus;
    struct spinlock_core_state locks;
    struct spinlock_irq_token first;
    struct spinlock_irq_token second;
    struct percpu_handle handle;
    bool restore = false;
    uint32_t index;

    percpu_core_clear(NULL);
    spinlock_core_clear(NULL);
    if (!initialize_pair(&cpus, &locks) ||
        percpu_core_validate(NULL) != PERCPU_STATUS_NULL_ARGUMENT ||
        spinlock_core_validate(NULL, &cpus) !=
            SPINLOCK_STATUS_NULL_ARGUMENT ||
        spinlock_core_validate(&locks, NULL) !=
            SPINLOCK_STATUS_NULL_ARGUMENT ||
        percpu_core_handle(&cpus, 0U, NULL) !=
            PERCPU_STATUS_NULL_ARGUMENT ||
        spinlock_core_irqsave_acquire(
            &locks, &cpus, 0U, 0U, true, NULL
        ) != SPINLOCK_STATUS_NULL_ARGUMENT) {
        return false;
    }

    cpus.reserved[0] = UINT8_C(1);
    if (percpu_core_validate(&cpus) != PERCPU_STATUS_CORRUPTED ||
        percpu_core_cpu_online(&cpus, 1U, 1U) !=
            PERCPU_STATUS_CORRUPTED ||
        percpu_core_validate(&cpus) != PERCPU_STATUS_POISONED) {
        return false;
    }

    if (!initialize_pair(&cpus, &locks) ||
        percpu_core_cpu_online(&cpus, 1U, 1U) != PERCPU_STATUS_OK ||
        spinlock_core_register(
            &locks, &cpus, 0U, SPINLOCK_CLASS_CPU_STATE, 0U
        ) != SPINLOCK_STATUS_OK ||
        spinlock_core_irqsave_acquire(
            &locks, &cpus, 0U, 0U, true, &first
        ) != SPINLOCK_STATUS_OK) {
        return false;
    }
    second = first;
    second.cpu_index = 1U;
    if (spinlock_core_irqrestore_release(
            &locks, &cpus, &second, false, &restore
        ) != SPINLOCK_STATUS_WRONG_OWNER ||
        spinlock_core_validate(&locks, &cpus) != SPINLOCK_STATUS_POISONED) {
        return false;
    }

    if (!initialize_pair(&cpus, &locks) ||
        spinlock_core_register(
            &locks, &cpus, 0U, SPINLOCK_CLASS_CPU_STATE, 0U
        ) != SPINLOCK_STATUS_OK ||
        spinlock_core_irqsave_acquire(
            &locks, &cpus, 0U, 0U, true, &first
        ) != SPINLOCK_STATUS_OK ||
        spinlock_core_irqsave_acquire(
            &locks, &cpus, 0U, 0U, false, &second
        ) != SPINLOCK_STATUS_RECURSIVE_LOCK ||
        spinlock_core_validate(&locks, &cpus) != SPINLOCK_STATUS_POISONED) {
        return false;
    }

    if (!initialize_pair(&cpus, &locks) ||
        spinlock_core_register(
            &locks, &cpus, 0U, SPINLOCK_CLASS_SCHEDULER, 0U
        ) != SPINLOCK_STATUS_OK ||
        spinlock_core_register(
            &locks, &cpus, 1U, SPINLOCK_CLASS_CPU_STATE, 0U
        ) != SPINLOCK_STATUS_OK ||
        spinlock_core_irqsave_acquire(
            &locks, &cpus, 0U, 0U, true, &first
        ) != SPINLOCK_STATUS_OK ||
        spinlock_core_irqsave_acquire(
            &locks, &cpus, 1U, 0U, false, &second
        ) != SPINLOCK_STATUS_ORDER_INVERSION ||
        spinlock_core_validate(&locks, &cpus) != SPINLOCK_STATUS_POISONED) {
        return false;
    }

    if (!initialize_pair(&cpus, &locks) ||
        spinlock_core_register(
            &locks, &cpus, 0U, SPINLOCK_CLASS_CPU_STATE, 0U
        ) != SPINLOCK_STATUS_OK ||
        spinlock_core_register(
            &locks, &cpus, 1U, SPINLOCK_CLASS_INTERRUPT, 0U
        ) != SPINLOCK_STATUS_OK ||
        spinlock_core_irqsave_acquire(
            &locks, &cpus, 0U, 0U, true, &first
        ) != SPINLOCK_STATUS_OK ||
        spinlock_core_irqsave_acquire(
            &locks, &cpus, 1U, 0U, true, &second
        ) != SPINLOCK_STATUS_INTERRUPTS_ENABLED ||
        spinlock_core_validate(&locks, &cpus) != SPINLOCK_STATUS_POISONED) {
        return false;
    }

    if (!initialize_pair(&cpus, &locks)) {
        return false;
    }
    for (index = 0U; index <= SPINLOCK_HELD_LIMIT; ++index) {
        if (spinlock_core_register(
                &locks, &cpus, index, SPINLOCK_CLASS_CPU_STATE,
                (uint8_t)index
            ) != SPINLOCK_STATUS_OK) {
            return false;
        }
    }
    for (index = 0U; index < SPINLOCK_HELD_LIMIT; ++index) {
        if (spinlock_core_irqsave_acquire(
                &locks, &cpus, index, 0U, index == 0U, &first
            ) != SPINLOCK_STATUS_OK) {
            return false;
        }
    }
    if (spinlock_core_irqsave_acquire(
            &locks, &cpus, SPINLOCK_HELD_LIMIT, 0U, false, &first
        ) != SPINLOCK_STATUS_NESTING_OVERFLOW ||
        spinlock_core_validate(&locks, &cpus) != SPINLOCK_STATUS_POISONED) {
        return false;
    }

    if (!initialize_pair(&cpus, &locks) ||
        spinlock_core_register(
            &locks, &cpus, 0U, SPINLOCK_CLASS_CPU_STATE, 0U
        ) != SPINLOCK_STATUS_OK ||
        spinlock_core_irqsave_acquire(
            &locks, &cpus, 0U, 0U, true, &first
        ) != SPINLOCK_STATUS_OK) {
        return false;
    }
    second = first;
    ++second.generation;
    if (spinlock_core_irqrestore_release(
            &locks, &cpus, &second, false, &restore
        ) != SPINLOCK_STATUS_STALE_TOKEN ||
        spinlock_core_validate(&locks, &cpus) != SPINLOCK_STATUS_POISONED) {
        return false;
    }

    if (!initialize_pair(&cpus, &locks) ||
        percpu_core_cpu_online(&cpus, 1U, 1U) != PERCPU_STATUS_OK ||
        spinlock_core_register(
            &locks, &cpus, 0U, SPINLOCK_CLASS_CPU_STATE, 0U
        ) != SPINLOCK_STATUS_OK ||
        spinlock_core_irqsave_acquire(
            &locks, &cpus, 0U, 1U, true, &first
        ) != SPINLOCK_STATUS_OK ||
        percpu_core_cpu_offline(&cpus, 1U) != PERCPU_STATUS_OK ||
        spinlock_core_validate(&locks, &cpus) != SPINLOCK_STATUS_CORRUPTED ||
        spinlock_core_register(
            &locks, &cpus, 1U, SPINLOCK_CLASS_INTERRUPT, 0U
        ) != SPINLOCK_STATUS_CORRUPTED ||
        spinlock_core_validate(&locks, &cpus) != SPINLOCK_STATUS_POISONED) {
        return false;
    }

    if (!initialize_pair(&cpus, &locks)) {
        return false;
    }
    cpus.next_generation = UINT32_MAX;
    if (percpu_core_cpu_online(&cpus, 1U, 1U) !=
            PERCPU_STATUS_GENERATION_EXHAUSTED ||
        percpu_core_validate(&cpus) != PERCPU_STATUS_POISONED) {
        return false;
    }

    if (!initialize_pair(&cpus, &locks) ||
        spinlock_core_register(
            &locks, &cpus, 0U, SPINLOCK_CLASS_CPU_STATE, 0U
        ) != SPINLOCK_STATUS_OK) {
        return false;
    }
    locks.next_generation = UINT32_MAX;
    if (spinlock_core_irqsave_acquire(
            &locks, &cpus, 0U, 0U, true, &first
        ) != SPINLOCK_STATUS_OK || first.generation != UINT32_MAX ||
        spinlock_core_irqrestore_release(
            &locks, &cpus, &first, false, &restore
        ) != SPINLOCK_STATUS_OK || !restore ||
        spinlock_core_irqsave_acquire(
            &locks, &cpus, 0U, 0U, true, &first
        ) != SPINLOCK_STATUS_OK ||
        first.generation != (uint64_t)UINT32_MAX + UINT64_C(1) ||
        spinlock_core_irqrestore_release(
            &locks, &cpus, &first, false, &restore
        ) != SPINLOCK_STATUS_OK ||
        spinlock_core_validate(&locks, &cpus) != SPINLOCK_STATUS_OK) {
        return false;
    }

    if (!initialize_pair(&cpus, &locks) ||
        spinlock_core_register(
            &locks, &cpus, 0U, SPINLOCK_CLASS_CPU_STATE, 0U
        ) != SPINLOCK_STATUS_OK) {
        return false;
    }
    locks.next_generation = UINT64_MAX;
    if (spinlock_core_irqsave_acquire(
            &locks, &cpus, 0U, 0U, true, &first
        ) != SPINLOCK_STATUS_GENERATION_EXHAUSTED ||
        spinlock_core_validate(&locks, &cpus) != SPINLOCK_STATUS_POISONED) {
        return false;
    }

    if (!initialize_pair(&cpus, &locks) ||
        percpu_core_handle(&cpus, 0U, &handle) != PERCPU_STATUS_OK ||
        handle.cpu_index != 0U || handle.generation == 0U) {
        return false;
    }
    return true;
}

int main(void)
{
    struct percpu_core_state cpus;
    struct spinlock_core_state locks;
    char operation;

    if (!self_test() || !initialize_pair(&cpus, &locks)) {
        return 2;
    }

    while (scanf(" %c", &operation) == 1) {
        unsigned int first;
        unsigned int second;
        unsigned int third;
        unsigned int fourth;

        switch (operation) {
        case 'C': {
            enum percpu_status cpu_status;
            enum spinlock_status lock_status;
            percpu_core_clear(&cpus);
            spinlock_core_clear(&locks);
            cpu_status = percpu_core_initialize(&cpus, 0U, 0U);
            lock_status = spinlock_core_initialize(&locks, &cpus);
            printf("C %d %d\n", (int)cpu_status, (int)lock_status);
            break;
        }
        case 'V':
            printf(
                "V %d %d\n",
                (int)percpu_core_validate(&cpus),
                (int)spinlock_core_validate(&locks, &cpus)
            );
            break;
        case 'P':
            if (scanf(" %u %u", &first, &second) != 2) {
                return 3;
            }
            printf(
                "P %d\n",
                (int)percpu_core_cpu_online(&cpus, first, second)
            );
            break;
        case 'F': {
            enum spinlock_status lock_status;
            enum percpu_status cpu_status = PERCPU_STATUS_CORRUPTED;
            if (scanf(" %u", &first) != 1) {
                return 3;
            }
            lock_status = spinlock_core_cpu_can_offline(&locks, &cpus, first);
            if (lock_status == SPINLOCK_STATUS_OK) {
                cpu_status = percpu_core_cpu_offline(&cpus, first);
            }
            printf("F %d %d\n", (int)lock_status, (int)cpu_status);
            break;
        }
        case 'L': {
            uint32_t cpu_index;
            enum percpu_status status;
            if (scanf(" %u", &first) != 1) {
                return 3;
            }
            status = percpu_core_lookup_apic(&cpus, first, &cpu_index);
            printf("L %d %u\n", (int)status, cpu_index);
            break;
        }
        case 'H': {
            struct percpu_handle handle;
            enum percpu_status status;
            if (scanf(" %u", &first) != 1) {
                return 3;
            }
            status = percpu_core_handle(&cpus, first, &handle);
            printf(
                "H %d %u %u\n", (int)status, handle.cpu_index,
                handle.generation
            );
            break;
        }
        case 'E': {
            struct percpu_handle handle;
            uint32_t cpu_index;
            enum percpu_status status;
            if (scanf(" %u %u", &first, &second) != 2) {
                return 3;
            }
            handle.cpu_index = first;
            handle.generation = second;
            status = percpu_core_resolve(&cpus, &handle, &cpu_index);
            printf("E %d %u\n", (int)status, cpu_index);
            break;
        }
        case 'R':
            if (scanf(" %u %u %u", &first, &second, &third) != 3) {
                return 3;
            }
            printf(
                "R %d\n",
                (int)spinlock_core_register(
                    &locks, &cpus, first,
                    (enum spinlock_order_class)second, (uint8_t)third
                )
            );
            break;
        case 'A': {
            struct spinlock_irq_token token;
            enum spinlock_status status;
            if (scanf(" %u %u %u", &first, &second, &third) != 3) {
                return 3;
            }
            status = spinlock_core_irqsave_acquire(
                &locks, &cpus, first, second, third != 0U, &token
            );
            printf(
                "A %d %u %u %" PRIu64 " %u\n", (int)status,
                token.cpu_index, token.lock_index, token.generation,
                token.interrupts_were_enabled ? 1U : 0U
            );
            break;
        }
        case 'U': {
            struct spinlock_irq_token token;
            enum spinlock_status status;
            bool restore;
            unsigned int fifth;
            uint64_t generation;
            if (scanf(
                    " %u %u %" SCNu64 " %u %u", &first, &second,
                    &generation,
                    &fourth, &fifth
                ) != 5) {
                return 3;
            }
            token.cpu_index = first;
            token.lock_index = second;
            token.generation = generation;
            token.interrupts_were_enabled = fourth != 0U;
            status = spinlock_core_irqrestore_release(
                &locks, &cpus, &token, fifth != 0U, &restore
            );
            printf("U %d %u\n", (int)status, restore ? 1U : 0U);
            break;
        }
        case 'O':
            if (scanf(" %u", &first) != 1) {
                return 3;
            }
            printf(
                "O %d\n",
                (int)spinlock_core_cpu_can_offline(&locks, &cpus, first)
            );
            break;
        case 'S': {
            struct spinlock_snapshot snapshot;
            enum spinlock_status status;
            if (scanf(" %u", &first) != 1) {
                return 3;
            }
            status = spinlock_core_snapshot(
                &locks, &cpus, first, &snapshot
            );
            printf(
                "S %d %u %" PRIu64 " %d %u %u %u\n", (int)status,
                snapshot.owner_cpu, snapshot.generation,
                (int)snapshot.order_class, snapshot.order_rank,
                snapshot.registered ? 1U : 0U,
                snapshot.locked ? 1U : 0U
            );
            break;
        }
        default:
            return 4;
        }
    }
    return ferror(stdin) == 0 ? 0 : 5;
}
