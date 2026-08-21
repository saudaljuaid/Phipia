/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pyrenis/dma.h>
#include <pyrenis/paging.h>

static struct dma_state state;

static enum dma_status validate_allocation(
    const struct dma_allocation *allocation
)
{
    if (allocation == NULL) {
        return DMA_STATUS_NULL_ARGUMENT;
    }

    if (!allocation->active || !allocation->frames.active ||
        allocation->cpu_address == NULL || allocation->byte_length == 0U ||
        allocation->owner <= DMA_OWNER_NONE ||
        allocation->owner >= DMA_OWNER_COUNT) {
        return DMA_STATUS_BAD_ALLOCATION;
    }

    return DMA_STATUS_OK;
}

enum dma_status dma_initialize(void)
{
    if (state.active) {
        return DMA_STATUS_ALREADY_INITIALIZED;
    }

    state.active_allocations = 0U;
    state.cpu_owned_allocations = 0U;
    state.device_owned_allocations = 0U;
    state.active = true;
    return DMA_STATUS_OK;
}

enum dma_status dma_allocate(
    const struct dma_request *request,
    struct dma_allocation *allocation
)
{
    struct frame_contiguous_request frame_request;
    struct paging_translation first;
    struct paging_translation last;
    enum frame_status frame_status;

    if (request == NULL || allocation == NULL) {
        return DMA_STATUS_NULL_ARGUMENT;
    }

    allocation->frames.active = false;
    allocation->cpu_address = NULL;
    allocation->byte_length = 0U;
    allocation->owner = DMA_OWNER_NONE;
    allocation->initialized = false;
    allocation->active = false;

    if (!state.active) {
        return DMA_STATUS_NOT_INITIALIZED;
    }

    frame_request.page_count = request->page_count;
    frame_request.alignment = request->alignment;
    frame_request.maximum_physical_address =
        request->maximum_physical_address;
    frame_status = frame_allocate_contiguous(&frame_request,
        &allocation->frames);
    if (frame_status != FRAME_STATUS_OK) {
        return DMA_STATUS_FRAME_ALLOCATION_FAILURE;
    }

    allocation->byte_length =
        (uint64_t)allocation->frames.page_count * PYRENIS_PAGE_SIZE;
    if (paging_translate((uint64_t)allocation->frames.physical_base, &first) !=
            PAGING_STATUS_OK ||
        paging_translate((uint64_t)allocation->frames.physical_base +
            allocation->byte_length - 1U, &last) != PAGING_STATUS_OK ||
        first.physical_address !=
            (uint64_t)allocation->frames.physical_base ||
        last.physical_address !=
            (uint64_t)allocation->frames.physical_base +
                allocation->byte_length - 1U ||
        first.memory_type != PAGING_MEMORY_WRITE_BACK ||
        last.memory_type != PAGING_MEMORY_WRITE_BACK ||
        (first.permissions & PAGING_WRITE) == 0U ||
        (last.permissions & PAGING_WRITE) == 0U) {
        (void)frame_release_contiguous(&allocation->frames);
        allocation->byte_length = 0U;
        return DMA_STATUS_NOT_IDENTITY_MAPPED;
    }

    allocation->cpu_address =
        (void *)(uintptr_t)allocation->frames.physical_base;
    allocation->owner = DMA_OWNER_CPU;
    allocation->active = true;
    ++state.active_allocations;
    ++state.cpu_owned_allocations;
    return DMA_STATUS_OK;
}

enum dma_status dma_mark_initialized(struct dma_allocation *allocation)
{
    const enum dma_status status = validate_allocation(allocation);

    if (status != DMA_STATUS_OK) {
        return status;
    }

    if (allocation->owner != DMA_OWNER_CPU) {
        return DMA_STATUS_WRONG_OWNER;
    }

    allocation->initialized = true;
    return DMA_STATUS_OK;
}

enum dma_status dma_transfer_to_device(struct dma_allocation *allocation)
{
    const enum dma_status status = validate_allocation(allocation);

    if (status != DMA_STATUS_OK) {
        return status;
    }

    if (allocation->owner != DMA_OWNER_CPU) {
        return DMA_STATUS_WRONG_OWNER;
    }

    if (!allocation->initialized) {
        return DMA_STATUS_NOT_PREPARED;
    }

    __asm__ volatile ("" : : : "memory");
    allocation->owner = DMA_OWNER_DEVICE;
    --state.cpu_owned_allocations;
    ++state.device_owned_allocations;
    return DMA_STATUS_OK;
}

enum dma_status dma_transfer_to_cpu(struct dma_allocation *allocation)
{
    const enum dma_status status = validate_allocation(allocation);

    if (status != DMA_STATUS_OK) {
        return status;
    }

    if (allocation->owner != DMA_OWNER_DEVICE) {
        return DMA_STATUS_WRONG_OWNER;
    }

    __asm__ volatile ("" : : : "memory");
    allocation->owner = DMA_OWNER_CPU;
    --state.device_owned_allocations;
    ++state.cpu_owned_allocations;
    return DMA_STATUS_OK;
}

enum dma_status dma_release(struct dma_allocation *allocation)
{
    enum frame_status frame_status;
    enum dma_status status;

    if (allocation == NULL) {
        return DMA_STATUS_NULL_ARGUMENT;
    }

    if (!state.active) {
        return DMA_STATUS_NOT_INITIALIZED;
    }

    if (!allocation->active) {
        return DMA_STATUS_DOUBLE_FREE;
    }

    status = validate_allocation(allocation);
    if (status != DMA_STATUS_OK) {
        return status;
    }

    if (allocation->owner != DMA_OWNER_CPU) {
        return DMA_STATUS_WRONG_OWNER;
    }

    frame_status = frame_release_contiguous(&allocation->frames);
    if (frame_status != FRAME_STATUS_OK) {
        return DMA_STATUS_BAD_ALLOCATION;
    }

    allocation->cpu_address = NULL;
    allocation->byte_length = 0U;
    allocation->owner = DMA_OWNER_NONE;
    allocation->initialized = false;
    allocation->active = false;
    --state.active_allocations;
    --state.cpu_owned_allocations;
    return DMA_STATUS_OK;
}

bool dma_is_device_owned(const struct dma_allocation *allocation)
{
    return allocation != NULL && allocation->active &&
        allocation->frames.active && allocation->initialized &&
        allocation->owner == DMA_OWNER_DEVICE;
}

struct dma_state dma_get_state(void)
{
    return state;
}

enum dma_status dma_verify(void)
{
    if (!state.active) {
        return DMA_STATUS_NOT_INITIALIZED;
    }

    if (state.cpu_owned_allocations + state.device_owned_allocations !=
            state.active_allocations) {
        return DMA_STATUS_BAD_ALLOCATION;
    }

    return DMA_STATUS_OK;
}

bool dma_self_test(void)
{
    struct dma_request request = {
        .page_count = 2U,
        .alignment = PYRENIS_PAGE_SIZE * 2U,
        .maximum_physical_address = UINT32_MAX
    };
    struct dma_allocation allocation;
    struct dma_state before;

    if (!state.active) {
        return false;
    }

    before = state;

    if (dma_allocate(NULL, &allocation) != DMA_STATUS_NULL_ARGUMENT ||
        dma_allocate(&request, NULL) != DMA_STATUS_NULL_ARGUMENT) {
        return false;
    }

    request.page_count = 0U;
    if (dma_allocate(&request, &allocation) !=
            DMA_STATUS_FRAME_ALLOCATION_FAILURE) {
        return false;
    }

    request.page_count = 1U;
    request.alignment = PYRENIS_PAGE_SIZE + 1U;
    if (dma_allocate(&request, &allocation) !=
            DMA_STATUS_FRAME_ALLOCATION_FAILURE) {
        return false;
    }

    request.alignment = PYRENIS_EARLY_PHYSICAL_LIMIT * 2U;
    if (dma_allocate(&request, &allocation) !=
            DMA_STATUS_FRAME_ALLOCATION_FAILURE) {
        return false;
    }

    request.page_count = 2U;
    request.alignment = PYRENIS_PAGE_SIZE * 2U;
    request.maximum_physical_address = UINT32_MAX;
    if (dma_allocate(&request, &allocation) != DMA_STATUS_OK ||
        ((uint64_t)allocation.frames.physical_base &
            (request.alignment - 1U)) != 0U ||
        (uint64_t)allocation.frames.physical_base +
            allocation.byte_length - 1U > request.maximum_physical_address) {
        return false;
    }

    if (dma_transfer_to_device(&allocation) != DMA_STATUS_NOT_PREPARED ||
        dma_mark_initialized(&allocation) != DMA_STATUS_OK ||
        dma_transfer_to_device(&allocation) != DMA_STATUS_OK ||
        dma_transfer_to_device(&allocation) != DMA_STATUS_WRONG_OWNER ||
        dma_mark_initialized(&allocation) != DMA_STATUS_WRONG_OWNER ||
        dma_release(&allocation) != DMA_STATUS_WRONG_OWNER ||
        dma_transfer_to_cpu(&allocation) != DMA_STATUS_OK ||
        dma_transfer_to_cpu(&allocation) != DMA_STATUS_WRONG_OWNER ||
        dma_release(&allocation) != DMA_STATUS_OK ||
        dma_release(&allocation) != DMA_STATUS_DOUBLE_FREE) {
        return false;
    }

    return state.active_allocations == before.active_allocations &&
        state.cpu_owned_allocations == before.cpu_owned_allocations &&
        state.device_owned_allocations == before.device_owned_allocations &&
        dma_verify() == DMA_STATUS_OK;
}

const char *dma_status_string(enum dma_status status)
{
    static const char *const messages[DMA_STATUS_COUNT] = {
        "ok",
        "null DMA argument",
        "DMA foundation was initialized twice",
        "DMA foundation is not initialized",
        "bounded contiguous frame allocation failed",
        "DMA allocation is not identity-mapped write-back memory",
        "DMA allocation was transferred before initialization",
        "DMA ownership transition came from the wrong owner",
        "DMA allocation was released twice",
        "DMA allocation handle is inconsistent"
    };

    _Static_assert(sizeof(messages) / sizeof(messages[0]) == DMA_STATUS_COUNT,
        "DMA status messages are out of sync");

    if (status < DMA_STATUS_OK || status >= DMA_STATUS_COUNT) {
        return "unknown DMA status";
    }

    return messages[status];
}
