/* SPDX-License-Identifier: GPL-3.0-only */
/* Production NVMe teardown with deterministic allocation-release failures. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/kernel/nvme.c"

static struct frame_allocator_stats frames;
static size_t allocations;
static unsigned releases;
static unsigned fail_release;
static bool interrupts = true;
static bool pci_live;

bool cpu_interrupts_enabled(void) { return interrupts; }
void cpu_interrupt_disable(void) { interrupts = false; }
void cpu_interrupt_enable(void) { interrupts = true; }
uint64_t clock_monotonic_ns(void) { assert(false); return 0U; }
enum pci_resource_status pci_claim_disable_bus_master(struct pci_device_claim *claim)
{ (void)claim; assert(false); return PCI_RESOURCE_STATUS_OK; }
enum pci_resource_status pci_release_device(struct pci_device_claim *claim)
{
    assert(!interrupts && pci_live && claim->active && allocations == 0U);
    claim->active = false;
    pci_live = false;
    return PCI_RESOURCE_STATUS_OK;
}
enum msix_status msix_unbind(struct msix_binding *binding)
{ (void)binding; assert(false); return MSIX_STATUS_OK; }
enum msix_status msix_set_masked(struct msix_binding *binding, bool masked)
{ (void)binding; (void)masked; assert(false); return MSIX_STATUS_OK; }
struct frame_allocator_stats frame_allocator_get_stats(void) { return frames; }
struct pci_resource_state pci_resource_get_state(void)
{ return (struct pci_resource_state){.active_claims = pci_live ? 1U : 0U}; }
struct interrupt_vector_state interrupt_vector_get_state(void) { return (struct interrupt_vector_state){0}; }
struct msix_state msix_get_state(void) { return (struct msix_state){0}; }
struct dma_state dma_get_state(void)
{
    return (struct dma_state){.active_allocations = allocations, .cpu_owned_allocations = allocations};
}
enum dma_status dma_transfer_to_cpu(struct dma_allocation *allocation)
{
    allocation->owner = DMA_OWNER_CPU;
    return DMA_STATUS_OK;
}
enum dma_status dma_release(struct dma_allocation *allocation)
{
    assert(!interrupts && allocation->active && allocation->owner == DMA_OWNER_CPU);
    if (++releases == fail_release) return DMA_STATUS_WRONG_OWNER;
    assert(allocations != 0U && frames.allocated_frames >= allocation->frames.page_count);
    --allocations;
    frames.allocated_frames -= allocation->frames.page_count;
    frames.free_frames += allocation->frames.page_count;
    memset(allocation, 0, sizeof(*allocation));
    return DMA_STATUS_OK;
}

int main(void)
{
    for (unsigned scenario = 0U; scenario < 6U; ++scenario) {
        const bool failed_open = scenario >= 3U;
        const unsigned failure = scenario % 3U + 1U;
        memset(&filesystem_runtime, 0, sizeof(filesystem_runtime));
        frames = (struct frame_allocator_stats){.addressable_frames = 100U,
            .allocatable_frames = 80U, .free_frames = 70U, .allocated_frames = 10U,
            .reserved_frames = 20U, .highest_allocatable_address = 0x100000U};
        filesystem_runtime.frames_before = frames;
        struct dma_allocation *owned[] = {&filesystem_runtime.controller.read.dma,
            &filesystem_runtime.controller.io.completion, &filesystem_runtime.controller.io.submission};
        for (size_t index = 0U; index < 3U; ++index) {
            owned[index]->active = true;
            owned[index]->owner = DMA_OWNER_CPU;
            owned[index]->frames.page_count = index + 1U;
            frames.allocated_frames += index + 1U;
            frames.free_frames -= index + 1U;
        }
        filesystem_runtime.frames_ready = frames;
        filesystem_runtime.active = true;
        filesystem_runtime.generation = 42U;
        filesystem_runtime.controller.claim.discovery.generation = 42U;
        filesystem_runtime.controller.claim.pci.active = true;
        pci_live = true;
        allocations = 3U;
        releases = 0U;
        fail_release = failure;
        // The filesystem client retains unrelated heap frames across close.
        if (!failed_open) {
            frames.allocated_frames += 2U;
            frames.free_frames -= 2U;
        }
        struct nvme_volume_session session = {.active = true, .generation = 42U,
            .state = NVME_FILESYSTEM_SESSION_READY};
        if (failed_open) {
            filesystem_runtime.controller.claim.discovery.generation = 0U;
            interrupts = false;
            assert(finish_volume_open(&session, 1U, true, NVME_STATUS_DMA_ALLOCATION) == NVME_STATUS_TEARDOWN_FAILURE);
            interrupts = true;
            assert(session.generation != 0U && volume_session_matches(&session));
            frames.allocated_frames += 2U;
            frames.free_frames -= 2U;
        } else {
            assert(nvme_volume_close(&session) == NVME_STATUS_TEARDOWN_FAILURE);
        }
        assert(interrupts && session.active && filesystem_runtime.active);
        assert(session.state == NVME_FILESYSTEM_SESSION_STOPPING);
        assert(allocations == 4U - failure);
        assert(pci_live && filesystem_runtime.controller.claim.pci.active);
        // Another client changes its frame ownership between retry attempts.
        frames.allocated_frames += 3U;
        frames.free_frames -= 3U;
        fail_release = releases + 1U;
        assert(nvme_volume_close(&session) == NVME_STATUS_TEARDOWN_FAILURE);
        assert(interrupts && session.active && filesystem_runtime.active);
        fail_release = 0U;
        assert(nvme_volume_close(&session) == NVME_STATUS_OK);
        assert(interrupts && !session.active && !filesystem_runtime.active);
        assert(allocations == 0U && frames.allocated_frames == 15U && frames.free_frames == 65U);
        assert(!pci_live);
        assert(session.close_resource_mismatches == 0U);
        assert(nvme_volume_close(&session) == NVME_STATUS_TRANSITION_REPEATED);
    }
    filesystem_runtime.active = true;
    filesystem_runtime.frames_before = frames;
    struct nvme_volume_session absent = {0};
    interrupts = false;
    assert(finish_volume_open(&absent, 1U, false, NVME_STATUS_ABSENT) == NVME_STATUS_ABSENT);
    interrupts = true;
    assert(!absent.active && !filesystem_runtime.active);
    // Matching the original global snapshot alone cannot prove an owned frame
    // was released during this attempt (another client may have freed it).
    filesystem_runtime.frames_before = frames;
    filesystem_runtime.frames_ready = frames;
    ++filesystem_runtime.frames_ready.allocated_frames;
    --filesystem_runtime.frames_ready.free_frames;
    assert((volume_resource_state_mismatches(&filesystem_runtime, frames) &
        NVME_VOLUME_RESOURCE_MISMATCH_FRAMES) != 0U);
    puts("ext4 NVMe failed-open retention, partial teardown, close retry and client frame isolation: PASS");
    return 0;
}
