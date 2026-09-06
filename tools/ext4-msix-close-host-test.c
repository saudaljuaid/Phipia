/* SPDX-License-Identifier: GPL-3.0-only */
/* Production MSI-X rollback used by ext4's NVMe ownership teardown. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/kernel/msix.c"

static unsigned failure;
static unsigned handlers;
static unsigned vectors;
static unsigned mappings;
static uint16_t control;

bool cpu_interrupts_enabled(void) { return false; }
void cpu_store_fence(void) {}
enum pci_status pci_config_write_port(struct pci_address address, uint16_t offset,
    size_t width, uint32_t value)
{
    (void)address;
    assert(offset == 0x42U && width == 2U);
    if (failure == ((value & MSIX_CONTROL_FUNCTION_MASK) != 0U ? 1U : 4U)) {
        return PCI_STATUS_NULL_ARGUMENT;
    }
    control = (uint16_t)value;
    return PCI_STATUS_OK;
}
enum pci_status pci_config_read_port(struct pci_address address, uint16_t offset, uint32_t *value)
{
    (void)address;
    assert(offset == 0x40U);
    if (failure == 5U) return PCI_STATUS_NULL_ARGUMENT;
    *value = (uint32_t)control << 16U;
    return PCI_STATUS_OK;
}
enum interrupt_status interrupt_unregister_handler(uint8_t vector)
{
    assert(vector == 64U && handlers == 1U);
    if (failure == 2U) return INTERRUPT_STATUS_BAD_ARGUMENT;
    --handlers;
    return INTERRUPT_STATUS_OK;
}
enum interrupt_vector_status interrupt_vector_release(struct interrupt_vector_allocation *allocation)
{
    assert(handlers == 0U && vectors == 1U && allocation->active);
    if (failure == 3U) return INTERRUPT_VECTOR_STATUS_STALE_ALLOCATION;
    --vectors;
    allocation->active = false;
    return INTERRUPT_VECTOR_STATUS_OK;
}
enum pci_resource_status pci_claim_unmap_last_bar(struct pci_device_claim *claim, uint8_t bar)
{
    assert(claim->active && handlers == 0U && vectors == 0U && control == 0U);
    assert((bar == 1U && mappings == 2U) || (bar == 0U && mappings == 1U));
    if (failure == (bar == 1U ? 6U : 7U)) return PCI_RESOURCE_STATUS_PAGING_FAILURE;
    --mappings;
    return PCI_RESOURCE_STATUS_OK;
}

int main(void)
{
    for (unsigned boundary = 1U; boundary <= 7U; ++boundary) {
        struct pci_device_claim claim = {.active = true};
        uint32_t entry[4] = {0xfee00000U, 0U, 64U, 0U};
        struct msix_binding binding = {.claim = &claim,
            .vector = {.vector = 64U, .generation = 1U, .active = true},
            .entry = entry, .capability_offset = 0x40U,
            .table_bar = 0U, .pba_bar = 1U, .handler_installed = true,
            .table_mapped_here = true, .pba_mapped_here = true, .active = true};
        memset(&state, 0, sizeof(state));
        state.active_bindings = 1U;
        handlers = vectors = 1U;
        mappings = 2U;
        control = MSIX_CONTROL_ENABLE;
        failure = boundary;
        for (unsigned retry = 0U; retry < 2U; ++retry) {
            assert(msix_unbind(&binding) == MSIX_STATUS_ROLLBACK_FAILURE);
            assert(binding.active && binding.teardown_started && state.active_bindings == 1U);
            assert(msix_set_masked(&binding, false) == MSIX_STATUS_ROLLBACK_FAILURE);
            assert(binding.entry == entry && binding.table_mapped_here);
            assert(mappings != 0U);
        }
        failure = 0U;
        assert(msix_unbind(&binding) == MSIX_STATUS_OK);
        assert(!binding.active && binding.entry == NULL && state.active_bindings == 0U);
        assert(handlers == 0U && vectors == 0U && mappings == 0U && control == 0U);
        assert(state.rollback_count == 1U);
        assert(msix_unbind(&binding) == MSIX_STATUS_NOT_BOUND);
    }
    puts("ext4 MSI-X teardown retains handlers, vectors and mappings across every failed release: PASS");
    return 0;
}
