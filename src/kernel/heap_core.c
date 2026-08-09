/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "heap_core.h"

#define HEAP_CORE_MAGIC UINT64_C(0x5A454E4954484845)
#define HEAP_CORE_DESCRIPTOR_SEED UINT64_C(0x9E3779B97F4A7C15)

static bool add_u64(uint64_t left, uint64_t right, uint64_t *sum)
{
    if (right > UINT64_MAX - left) {
        return false;
    }

    *sum = left + right;
    return true;
}

static bool alignment_is_valid(uint64_t alignment)
{
    return alignment != 0U &&
        alignment <= (uint64_t)HEAP_MAXIMUM_ALIGNMENT &&
        (alignment & (alignment - 1U)) == 0U;
}

static uint64_t effective_alignment(uint64_t alignment)
{
    if (alignment < (uint64_t)HEAP_MINIMUM_ALIGNMENT) {
        return (uint64_t)HEAP_MINIMUM_ALIGNMENT;
    }

    return alignment;
}

static bool align_up_u64(
    uint64_t value,
    uint64_t alignment,
    uint64_t *aligned
)
{
    const uint64_t mask = alignment - 1U;

    if (value > UINT64_MAX - mask) {
        return false;
    }

    *aligned = (value + mask) & ~mask;
    return true;
}

static uint64_t hash_word(uint64_t hash, uint64_t word)
{
    hash ^= word;
    hash *= UINT64_C(0x100000001B3);
    hash ^= hash >> 32U;
    return hash;
}

static uint64_t descriptor_integrity(
    const struct heap_core_descriptor *descriptor,
    uint32_t index
)
{
    uint64_t hash = HEAP_CORE_DESCRIPTOR_SEED ^ (uint64_t)index;

    hash = hash_word(hash, descriptor->offset);
    hash = hash_word(hash, descriptor->extent_size);
    hash = hash_word(hash, descriptor->requested_size);
    hash = hash_word(hash, descriptor->alignment);
    hash = hash_word(hash, (uint64_t)descriptor->previous);
    hash = hash_word(hash, (uint64_t)descriptor->next);
    hash = hash_word(hash, (uint64_t)descriptor->state);
    hash = hash_word(hash, (uint64_t)descriptor->reserved);
    return hash;
}

static void refresh_descriptor(
    struct heap_core_state *state,
    uint32_t index
)
{
    state->descriptors[index].integrity = descriptor_integrity(
        &state->descriptors[index],
        index
    );
}

static void make_unused(struct heap_core_state *state, uint32_t index)
{
    struct heap_core_descriptor *descriptor = &state->descriptors[index];

    descriptor->offset = 0U;
    descriptor->extent_size = 0U;
    descriptor->requested_size = 0U;
    descriptor->alignment = 0U;
    descriptor->previous = HEAP_CORE_INDEX_NONE;
    descriptor->next = HEAP_CORE_INDEX_NONE;
    descriptor->state = HEAP_CORE_DESCRIPTOR_UNUSED;
    descriptor->reserved = 0U;
    refresh_descriptor(state, index);
}

static void make_free(
    struct heap_core_state *state,
    uint32_t index,
    uint64_t offset,
    uint64_t extent_size,
    uint32_t previous,
    uint32_t next
)
{
    struct heap_core_descriptor *descriptor = &state->descriptors[index];

    descriptor->offset = offset;
    descriptor->extent_size = extent_size;
    descriptor->requested_size = 0U;
    descriptor->alignment = 0U;
    descriptor->previous = previous;
    descriptor->next = next;
    descriptor->state = HEAP_CORE_DESCRIPTOR_FREE;
    descriptor->reserved = 0U;
    refresh_descriptor(state, index);
}

static void make_live(
    struct heap_core_state *state,
    uint32_t index,
    uint64_t offset,
    uint64_t extent_size,
    uint64_t requested_size,
    uint64_t alignment,
    uint32_t previous,
    uint32_t next
)
{
    struct heap_core_descriptor *descriptor = &state->descriptors[index];

    descriptor->offset = offset;
    descriptor->extent_size = extent_size;
    descriptor->requested_size = requested_size;
    descriptor->alignment = alignment;
    descriptor->previous = previous;
    descriptor->next = next;
    descriptor->state = HEAP_CORE_DESCRIPTOR_LIVE;
    descriptor->reserved = 0U;
    refresh_descriptor(state, index);
}

static bool descriptor_is_intact(
    const struct heap_core_state *state,
    uint32_t index
)
{
    return state->descriptors[index].integrity == descriptor_integrity(
        &state->descriptors[index],
        index
    );
}

static uint32_t find_unused(
    const struct heap_core_state *state,
    uint32_t excluded
)
{
    for (uint32_t index = 0U; index < state->descriptor_limit; ++index) {
        if (index != excluded &&
            state->descriptors[index].state == HEAP_CORE_DESCRIPTOR_UNUSED) {
            return index;
        }
    }

    return HEAP_CORE_INDEX_NONE;
}

static enum heap_core_status observe(
    const struct heap_core_state *state,
    struct heap_core_stats *stats
)
{
    uint64_t expected_offset = 0U;
    uint64_t observed_live_allocations = 0U;
    uint64_t observed_live_requested = 0U;
    uint64_t observed_live_extent = 0U;
    uint64_t observed_reusable = 0U;
    uint64_t largest_reusable = 0U;
    uint32_t active_descriptors = 0U;
    uint32_t linked_descriptors = 0U;
    uint32_t previous = HEAP_CORE_INDEX_NONE;
    uint32_t current;
    bool previous_was_free = false;

    if (state == NULL) {
        return HEAP_CORE_STATUS_NULL_ARGUMENT;
    }

    if (state->magic != HEAP_CORE_MAGIC ||
        state->inverse_magic != ~HEAP_CORE_MAGIC) {
        return HEAP_CORE_STATUS_NOT_INITIALIZED;
    }

    if (state->capacity == 0U || state->committed > state->capacity ||
        state->descriptor_limit != (uint32_t)HEAP_DESCRIPTOR_LIMIT) {
        return HEAP_CORE_STATUS_CORRUPTED_STATE;
    }

    for (uint32_t index = 0U; index < state->descriptor_limit; ++index) {
        const struct heap_core_descriptor *descriptor =
            &state->descriptors[index];

        if (!descriptor_is_intact(state, index) || descriptor->reserved != 0U) {
            return HEAP_CORE_STATUS_CORRUPTED_STATE;
        }

        if (descriptor->state == HEAP_CORE_DESCRIPTOR_UNUSED) {
            if (descriptor->offset != 0U || descriptor->extent_size != 0U ||
                descriptor->requested_size != 0U ||
                descriptor->alignment != 0U ||
                descriptor->previous != HEAP_CORE_INDEX_NONE ||
                descriptor->next != HEAP_CORE_INDEX_NONE) {
                return HEAP_CORE_STATUS_CORRUPTED_STATE;
            }

            continue;
        }

        if (descriptor->state != HEAP_CORE_DESCRIPTOR_FREE &&
            descriptor->state != HEAP_CORE_DESCRIPTOR_LIVE) {
            return HEAP_CORE_STATUS_CORRUPTED_STATE;
        }

        ++active_descriptors;
    }

    if (state->committed == 0U) {
        if (state->head != HEAP_CORE_INDEX_NONE || active_descriptors != 0U) {
            return HEAP_CORE_STATUS_CORRUPTED_STATE;
        }
    } else if (state->head >= state->descriptor_limit) {
        return HEAP_CORE_STATUS_CORRUPTED_STATE;
    }

    current = state->head;

    while (current != HEAP_CORE_INDEX_NONE) {
        const struct heap_core_descriptor *descriptor;
        uint64_t end;

        if (current >= state->descriptor_limit ||
            linked_descriptors >= state->descriptor_limit) {
            return HEAP_CORE_STATUS_CORRUPTED_STATE;
        }

        descriptor = &state->descriptors[current];

        if (descriptor->state == HEAP_CORE_DESCRIPTOR_UNUSED ||
            descriptor->previous != previous || descriptor->extent_size == 0U ||
            descriptor->offset != expected_offset ||
            !add_u64(descriptor->offset, descriptor->extent_size, &end) ||
            end > state->committed) {
            return HEAP_CORE_STATUS_CORRUPTED_STATE;
        }

        if (descriptor->state == HEAP_CORE_DESCRIPTOR_FREE) {
            if (descriptor->requested_size != 0U ||
                descriptor->alignment != 0U || previous_was_free ||
                !add_u64(
                    observed_reusable,
                    descriptor->extent_size,
                    &observed_reusable
                )) {
                return HEAP_CORE_STATUS_CORRUPTED_STATE;
            }

            if (descriptor->extent_size > largest_reusable) {
                largest_reusable = descriptor->extent_size;
            }

            previous_was_free = true;
        } else {
            if (descriptor->requested_size == 0U ||
                descriptor->requested_size > descriptor->extent_size ||
                !alignment_is_valid(descriptor->alignment) ||
                descriptor->alignment < (uint64_t)HEAP_MINIMUM_ALIGNMENT ||
                (descriptor->offset & (descriptor->alignment - 1U)) != 0U ||
                !add_u64(
                    observed_live_requested,
                    descriptor->requested_size,
                    &observed_live_requested
                ) ||
                !add_u64(
                    observed_live_extent,
                    descriptor->extent_size,
                    &observed_live_extent
                )) {
                return HEAP_CORE_STATUS_CORRUPTED_STATE;
            }

            ++observed_live_allocations;
            previous_was_free = false;
        }

        expected_offset = end;
        previous = current;
        current = descriptor->next;
        ++linked_descriptors;
    }

    if (expected_offset != state->committed ||
        linked_descriptors != active_descriptors ||
        observed_live_allocations != state->live_allocations ||
        observed_live_requested != state->live_requested_bytes ||
        observed_live_extent != state->live_extent_bytes ||
        observed_reusable > state->committed ||
        observed_live_extent != state->committed - observed_reusable) {
        return HEAP_CORE_STATUS_STATS_INVALID;
    }

    if (stats != NULL) {
        stats->capacity = state->capacity;
        stats->committed = state->committed;
        stats->live_allocations = observed_live_allocations;
        stats->live_requested_bytes = observed_live_requested;
        stats->live_extent_bytes = observed_live_extent;
        stats->reusable_bytes = observed_reusable;
        stats->largest_reusable_block = largest_reusable;
        stats->descriptors_in_use = active_descriptors;
        stats->descriptor_limit = state->descriptor_limit;
    }

    return HEAP_CORE_STATUS_OK;
}

void heap_core_clear(struct heap_core_state *state)
{
    uint8_t *bytes = (uint8_t *)state;

    if (state == NULL) {
        return;
    }

    for (size_t index = 0U; index < sizeof(*state); ++index) {
        bytes[index] = 0U;
    }
}

void heap_core_copy(
    struct heap_core_state *destination,
    const struct heap_core_state *source
)
{
    uint8_t *destination_bytes = (uint8_t *)destination;
    const uint8_t *source_bytes = (const uint8_t *)source;

    if (destination == NULL || source == NULL || destination == source) {
        return;
    }

    for (size_t index = 0U; index < sizeof(*destination); ++index) {
        destination_bytes[index] = source_bytes[index];
    }
}

enum heap_core_status heap_core_initialize(
    struct heap_core_state *state,
    uint64_t capacity
)
{
    if (state == NULL) {
        return HEAP_CORE_STATUS_NULL_ARGUMENT;
    }

    if (state->magic == HEAP_CORE_MAGIC &&
        state->inverse_magic == ~HEAP_CORE_MAGIC) {
        return heap_core_validate(state) == HEAP_CORE_STATUS_OK
            ? HEAP_CORE_STATUS_ALREADY_INITIALIZED
            : HEAP_CORE_STATUS_CORRUPTED_STATE;
    }

    if (state->magic != 0U || state->inverse_magic != 0U) {
        return HEAP_CORE_STATUS_CORRUPTED_STATE;
    }

    if (capacity == 0U) {
        return HEAP_CORE_STATUS_BAD_CAPACITY;
    }

    heap_core_clear(state);
    state->magic = HEAP_CORE_MAGIC;
    state->inverse_magic = ~HEAP_CORE_MAGIC;
    state->capacity = capacity;
    state->head = HEAP_CORE_INDEX_NONE;
    state->descriptor_limit = (uint32_t)HEAP_DESCRIPTOR_LIMIT;

    for (uint32_t index = 0U; index < state->descriptor_limit; ++index) {
        make_unused(state, index);
    }

    return heap_core_validate(state);
}

enum heap_core_status heap_core_validate(const struct heap_core_state *state)
{
    return observe(state, NULL);
}

enum heap_core_status heap_core_grow(
    struct heap_core_state *state,
    uint64_t additional_bytes
)
{
    uint64_t new_committed;
    uint32_t tail;
    enum heap_core_status status = heap_core_validate(state);

    if (status != HEAP_CORE_STATUS_OK) {
        return status;
    }

    if (additional_bytes == 0U ||
        !add_u64(state->committed, additional_bytes, &new_committed) ||
        new_committed > state->capacity) {
        return additional_bytes == 0U
            ? HEAP_CORE_STATUS_ZERO_SIZE
            : HEAP_CORE_STATUS_OUT_OF_SPACE;
    }

    if (state->head == HEAP_CORE_INDEX_NONE) {
        tail = find_unused(state, HEAP_CORE_INDEX_NONE);

        if (tail == HEAP_CORE_INDEX_NONE) {
            return HEAP_CORE_STATUS_DESCRIPTOR_LIMIT;
        }

        make_free(
            state,
            tail,
            0U,
            additional_bytes,
            HEAP_CORE_INDEX_NONE,
            HEAP_CORE_INDEX_NONE
        );
        state->head = tail;
    } else {
        tail = state->head;

        for (uint32_t steps = 0U; steps < state->descriptor_limit; ++steps) {
            uint32_t next = state->descriptors[tail].next;

            if (next == HEAP_CORE_INDEX_NONE) {
                break;
            }

            tail = next;
        }

        if (state->descriptors[tail].next != HEAP_CORE_INDEX_NONE) {
            return HEAP_CORE_STATUS_CORRUPTED_STATE;
        }

        if (state->descriptors[tail].state == HEAP_CORE_DESCRIPTOR_FREE) {
            state->descriptors[tail].extent_size += additional_bytes;
            refresh_descriptor(state, tail);
        } else {
            uint32_t added = find_unused(state, HEAP_CORE_INDEX_NONE);

            if (added == HEAP_CORE_INDEX_NONE) {
                return HEAP_CORE_STATUS_DESCRIPTOR_LIMIT;
            }

            make_free(
                state,
                added,
                state->committed,
                additional_bytes,
                tail,
                HEAP_CORE_INDEX_NONE
            );
            state->descriptors[tail].next = added;
            refresh_descriptor(state, tail);
        }
    }

    state->committed = new_committed;
    return heap_core_validate(state);
}

enum heap_core_status heap_core_growth_needed(
    const struct heap_core_state *state,
    uint64_t byte_size,
    uint64_t alignment,
    uint64_t *additional_bytes
)
{
    uint64_t start;
    uint64_t aligned_start;
    uint64_t required_end;
    uint64_t normalized_alignment;
    uint32_t tail;
    enum heap_core_status status;

    if (additional_bytes == NULL) {
        return HEAP_CORE_STATUS_NULL_ARGUMENT;
    }

    status = heap_core_validate(state);

    if (status != HEAP_CORE_STATUS_OK) {
        return status;
    }

    if (byte_size == 0U) {
        return HEAP_CORE_STATUS_ZERO_SIZE;
    }

    if (!alignment_is_valid(alignment)) {
        return HEAP_CORE_STATUS_INVALID_ALIGNMENT;
    }

    start = state->committed;
    tail = state->head;

    if (tail != HEAP_CORE_INDEX_NONE) {
        for (uint32_t steps = 0U; steps < state->descriptor_limit; ++steps) {
            uint32_t next = state->descriptors[tail].next;

            if (next == HEAP_CORE_INDEX_NONE) {
                break;
            }

            tail = next;
        }

        if (state->descriptors[tail].state == HEAP_CORE_DESCRIPTOR_FREE) {
            start = state->descriptors[tail].offset;
        }
    }

    normalized_alignment = effective_alignment(alignment);

    if (!align_up_u64(start, normalized_alignment, &aligned_start) ||
        !add_u64(aligned_start, byte_size, &required_end)) {
        return HEAP_CORE_STATUS_ARITHMETIC_OVERFLOW;
    }

    if (required_end > state->capacity) {
        return HEAP_CORE_STATUS_OUT_OF_SPACE;
    }

    *additional_bytes = required_end > state->committed
        ? required_end - state->committed
        : 0U;
    return HEAP_CORE_STATUS_OK;
}

enum heap_core_status heap_core_allocate(
    struct heap_core_state *state,
    uint64_t byte_size,
    uint64_t alignment,
    uint64_t *offset
)
{
    uint64_t normalized_alignment;
    uint32_t current;
    bool descriptor_blocked = false;
    enum heap_core_status status;

    if (offset == NULL) {
        return HEAP_CORE_STATUS_NULL_ARGUMENT;
    }

    status = heap_core_validate(state);

    if (status != HEAP_CORE_STATUS_OK) {
        return status;
    }

    if (byte_size == 0U) {
        return HEAP_CORE_STATUS_ZERO_SIZE;
    }

    if (!alignment_is_valid(alignment)) {
        return HEAP_CORE_STATUS_INVALID_ALIGNMENT;
    }

    normalized_alignment = effective_alignment(alignment);
    current = state->head;

    for (uint32_t steps = 0U;
         current != HEAP_CORE_INDEX_NONE && steps < state->descriptor_limit;
         ++steps) {
        struct heap_core_descriptor *descriptor = &state->descriptors[current];
        uint64_t aligned_start;
        uint64_t allocation_end;
        uint64_t block_end;
        uint64_t prefix_size;
        uint64_t suffix_size;
        uint32_t live_index = current;
        uint32_t suffix_index = HEAP_CORE_INDEX_NONE;
        uint32_t old_next;
        uint32_t old_previous;

        if (descriptor->state != HEAP_CORE_DESCRIPTOR_FREE) {
            current = descriptor->next;
            continue;
        }

        if (!align_up_u64(
                descriptor->offset,
                normalized_alignment,
                &aligned_start
            ) ||
            !add_u64(aligned_start, byte_size, &allocation_end) ||
            !add_u64(
                descriptor->offset,
                descriptor->extent_size,
                &block_end
            )) {
            return HEAP_CORE_STATUS_ARITHMETIC_OVERFLOW;
        }

        if (allocation_end > block_end) {
            current = descriptor->next;
            continue;
        }

        prefix_size = aligned_start - descriptor->offset;
        suffix_size = block_end - allocation_end;

        if (prefix_size != 0U) {
            live_index = find_unused(state, HEAP_CORE_INDEX_NONE);

            if (live_index == HEAP_CORE_INDEX_NONE) {
                descriptor_blocked = true;
                current = descriptor->next;
                continue;
            }
        }

        if (suffix_size != 0U) {
            suffix_index = find_unused(state, live_index);

            if (suffix_index == HEAP_CORE_INDEX_NONE) {
                descriptor_blocked = true;
                current = descriptor->next;
                continue;
            }
        }

        old_next = descriptor->next;
        old_previous = descriptor->previous;

        if (prefix_size == 0U) {
            make_live(
                state,
                current,
                aligned_start,
                byte_size,
                byte_size,
                normalized_alignment,
                old_previous,
                suffix_index != HEAP_CORE_INDEX_NONE
                    ? suffix_index
                    : old_next
            );
        } else {
            make_free(
                state,
                current,
                descriptor->offset,
                prefix_size,
                old_previous,
                live_index
            );
            make_live(
                state,
                live_index,
                aligned_start,
                byte_size,
                byte_size,
                normalized_alignment,
                current,
                suffix_index != HEAP_CORE_INDEX_NONE
                    ? suffix_index
                    : old_next
            );
        }

        if (suffix_index != HEAP_CORE_INDEX_NONE) {
            make_free(
                state,
                suffix_index,
                allocation_end,
                suffix_size,
                live_index,
                old_next
            );
        }

        if (old_next != HEAP_CORE_INDEX_NONE) {
            state->descriptors[old_next].previous =
                suffix_index != HEAP_CORE_INDEX_NONE
                    ? suffix_index
                    : live_index;
            refresh_descriptor(state, old_next);
        }

        ++state->live_allocations;
        state->live_requested_bytes += byte_size;
        state->live_extent_bytes += byte_size;
        *offset = aligned_start;
        return heap_core_validate(state);
    }

    return descriptor_blocked
        ? HEAP_CORE_STATUS_DESCRIPTOR_LIMIT
        : HEAP_CORE_STATUS_OUT_OF_SPACE;
}

enum heap_core_status heap_core_deallocate(
    struct heap_core_state *state,
    uint64_t offset
)
{
    struct heap_core_descriptor *descriptor;
    uint64_t requested_size;
    uint64_t extent_size;
    uint32_t current;
    enum heap_core_status status = heap_core_validate(state);

    if (status != HEAP_CORE_STATUS_OK) {
        return status;
    }

    if ((offset & ((uint64_t)HEAP_MINIMUM_ALIGNMENT - 1U)) != 0U) {
        return HEAP_CORE_STATUS_MISALIGNED_POINTER;
    }

    if (offset >= state->committed) {
        return HEAP_CORE_STATUS_POINTER_OUTSIDE;
    }

    current = state->head;

    for (uint32_t steps = 0U;
         current != HEAP_CORE_INDEX_NONE && steps < state->descriptor_limit;
         ++steps) {
        uint64_t end;

        descriptor = &state->descriptors[current];

        if (!add_u64(descriptor->offset, descriptor->extent_size, &end)) {
            return HEAP_CORE_STATUS_CORRUPTED_STATE;
        }

        if (offset == descriptor->offset) {
            if (descriptor->state == HEAP_CORE_DESCRIPTOR_FREE) {
                return HEAP_CORE_STATUS_DOUBLE_FREE;
            }

            break;
        }

        if (offset > descriptor->offset && offset < end) {
            return descriptor->state == HEAP_CORE_DESCRIPTOR_FREE
                ? HEAP_CORE_STATUS_DOUBLE_FREE
                : HEAP_CORE_STATUS_INTERIOR_POINTER;
        }

        current = descriptor->next;
    }

    if (current == HEAP_CORE_INDEX_NONE) {
        return HEAP_CORE_STATUS_INVALID_POINTER;
    }

    descriptor = &state->descriptors[current];
    requested_size = descriptor->requested_size;
    extent_size = descriptor->extent_size;
    make_free(
        state,
        current,
        descriptor->offset,
        descriptor->extent_size,
        descriptor->previous,
        descriptor->next
    );

    if (state->descriptors[current].previous != HEAP_CORE_INDEX_NONE) {
        uint32_t previous = state->descriptors[current].previous;

        if (state->descriptors[previous].state == HEAP_CORE_DESCRIPTOR_FREE) {
            uint32_t next = state->descriptors[current].next;

            state->descriptors[previous].extent_size +=
                state->descriptors[current].extent_size;
            state->descriptors[previous].next = next;
            refresh_descriptor(state, previous);

            if (next != HEAP_CORE_INDEX_NONE) {
                state->descriptors[next].previous = previous;
                refresh_descriptor(state, next);
            }

            make_unused(state, current);
            current = previous;
        }
    }

    if (state->descriptors[current].next != HEAP_CORE_INDEX_NONE) {
        uint32_t next = state->descriptors[current].next;

        if (state->descriptors[next].state == HEAP_CORE_DESCRIPTOR_FREE) {
            uint32_t following = state->descriptors[next].next;

            state->descriptors[current].extent_size +=
                state->descriptors[next].extent_size;
            state->descriptors[current].next = following;
            refresh_descriptor(state, current);

            if (following != HEAP_CORE_INDEX_NONE) {
                state->descriptors[following].previous = current;
                refresh_descriptor(state, following);
            }

            make_unused(state, next);
        }
    }

    --state->live_allocations;
    state->live_requested_bytes -= requested_size;
    state->live_extent_bytes -= extent_size;
    return heap_core_validate(state);
}

enum heap_core_status heap_core_get_stats(
    const struct heap_core_state *state,
    struct heap_core_stats *stats
)
{
    if (stats == NULL) {
        return HEAP_CORE_STATUS_NULL_ARGUMENT;
    }

    return observe(state, stats);
}

const char *heap_core_status_string(enum heap_core_status status)
{
    switch (status) {
    case HEAP_CORE_STATUS_OK:
        return "ok";
    case HEAP_CORE_STATUS_NULL_ARGUMENT:
        return "null heap-core argument";
    case HEAP_CORE_STATUS_ALREADY_INITIALIZED:
        return "heap core is already initialized";
    case HEAP_CORE_STATUS_NOT_INITIALIZED:
        return "heap core is not initialized";
    case HEAP_CORE_STATUS_BAD_CAPACITY:
        return "heap-core capacity is invalid";
    case HEAP_CORE_STATUS_ZERO_SIZE:
        return "heap-core size is zero";
    case HEAP_CORE_STATUS_INVALID_ALIGNMENT:
        return "heap-core alignment is invalid";
    case HEAP_CORE_STATUS_ARITHMETIC_OVERFLOW:
        return "heap-core arithmetic overflows";
    case HEAP_CORE_STATUS_OUT_OF_SPACE:
        return "heap-core capacity is exhausted";
    case HEAP_CORE_STATUS_DESCRIPTOR_LIMIT:
        return "heap-core descriptor limit is exhausted";
    case HEAP_CORE_STATUS_POINTER_OUTSIDE:
        return "pointer is outside committed heap memory";
    case HEAP_CORE_STATUS_MISALIGNED_POINTER:
        return "heap pointer is misaligned";
    case HEAP_CORE_STATUS_INTERIOR_POINTER:
        return "heap pointer is interior to an allocation";
    case HEAP_CORE_STATUS_INVALID_POINTER:
        return "heap pointer has no allocation provenance";
    case HEAP_CORE_STATUS_DOUBLE_FREE:
        return "heap allocation was freed twice";
    case HEAP_CORE_STATUS_CORRUPTED_STATE:
        return "heap-core metadata is corrupted";
    case HEAP_CORE_STATUS_STATS_INVALID:
        return "heap-core statistics are inconsistent";
    default:
        return "unknown heap-core status";
    }
}
