/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Intel High Definition Audio: the controller, its two command rings, and the
 * codecs on the other end of them.
 *
 * This is the first Sapote driver whose device writes into kernel memory for
 * something other than storage or networking, and the ordering that makes that
 * safe is the whole point of the file. Sapote has no IOMMU. A device with bus
 * mastering enabled can write anywhere, so bus mastering is enabled only after
 * both rings are typed DMA allocations declared to the claim, and it is
 * withdrawn only after the ring engines have been stopped and the controller
 * put back into reset. Memory is reclaimed after that, never before.
 *
 * The conversation itself is the ordinary one. Software writes a verb into the
 * command ring and advances the write pointer; the controller reads it, puts
 * it on the link, and writes whatever the codec answers into the response ring,
 * advancing its own write pointer. Reading that pointer is how a driver knows
 * an answer arrived, and it is the only thing in this file that waits.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sapote/audio.h>
#include <sapote/clock.h>
#include <sapote/cpu.h>
#include <sapote/dma.h>
#include <sapote/interrupt_vector.h>
#include <sapote/memory.h>
#include <sapote/msix.h>
#include <sapote/paging.h>
#include <sapote/pci.h>
#include <sapote/pci_resource.h>

/* PCI Code and ID Assignment Specification 1.19: multimedia, HD Audio. */
#define AUDIO_PCI_VENDOR UINT16_C(0x8086)
#define AUDIO_PCI_DEVICE UINT16_C(0x293E)
#define AUDIO_PCI_CLASS UINT8_C(0x04)
#define AUDIO_PCI_SUBCLASS UINT8_C(0x03)
#define AUDIO_REGISTER_BAR 0U
#define AUDIO_MINIMUM_REGISTER_BYTES UINT64_C(0x100)

/* High Definition Audio Specification 1.0a, section 3.3. */
#define HDA_GCAP UINT64_C(0x00)
#define HDA_VMIN UINT64_C(0x02)
#define HDA_VMAJ UINT64_C(0x03)
#define HDA_GCTL UINT64_C(0x08)
#define HDA_STATESTS UINT64_C(0x0E)
#define HDA_CORBLBASE UINT64_C(0x40)
#define HDA_CORBUBASE UINT64_C(0x44)
#define HDA_CORBWP UINT64_C(0x48)
#define HDA_CORBRP UINT64_C(0x4A)
#define HDA_CORBCTL UINT64_C(0x4C)
#define HDA_CORBSIZE UINT64_C(0x4E)
#define HDA_RIRBLBASE UINT64_C(0x50)
#define HDA_RIRBUBASE UINT64_C(0x54)
#define HDA_RIRBWP UINT64_C(0x58)
#define HDA_RINTCNT UINT64_C(0x5A)
#define HDA_RIRBCTL UINT64_C(0x5C)
#define HDA_RIRBSTS UINT64_C(0x5D)
#define HDA_RIRBSIZE UINT64_C(0x5E)

#define HDA_GCTL_CONTROLLER_RESET UINT32_C(0x00000001)
#define HDA_CORBRP_RESET UINT16_C(0x8000)
#define HDA_CORBCTL_DMA_ENABLE UINT8_C(0x02)
#define HDA_RIRBWP_RESET UINT16_C(0x8000)
#define HDA_RIRBCTL_DMA_ENABLE UINT8_C(0x02)

/*
 * Section 3.3.28 and 3.3.29: the controller counts responses and stops taking
 * commands once it has accumulated the number the interrupt count names, until
 * software acknowledges the response-interrupt flag. A driver that polls the
 * response ring rather than taking that interrupt has to do both things: set
 * the threshold beyond the number of answers it will ever collect in one
 * conversation, and acknowledge the flag after every answer anyway, because a
 * threshold is a bound and not a promise.
 */
#define HDA_RESPONSE_INTERRUPT_COUNT UINT16_C(0x00FF)
#define HDA_RIRBSTS_RESPONSE_INTERRUPT UINT8_C(0x01)
#define HDA_RIRBSTS_OVERRUN UINT8_C(0x04)
#define HDA_RIRBSTS_ACKNOWLEDGE \
    (HDA_RIRBSTS_RESPONSE_INTERRUPT | HDA_RIRBSTS_OVERRUN)

/*
 * Section 3.3.24 and 3.3.30: the low two bits select the ring size and bits
 * 6:4 report which sizes the controller supports - two entries, sixteen, or
 * two hundred and fifty six.
 */
#define HDA_RING_SIZE_SELECT_MASK UINT8_C(0x03)
#define HDA_RING_SIZE_CAPABILITY_SHIFT 4U
#define HDA_RING_SIZE_SUPPORTS_2 UINT8_C(0x01)
#define HDA_RING_SIZE_SUPPORTS_16 UINT8_C(0x02)
#define HDA_RING_SIZE_SUPPORTS_256 UINT8_C(0x04)

/* Section 3.3.2: the global capabilities field, taken apart. */
#define HDA_GCAP_OUTPUT_SHIFT 12U
#define HDA_GCAP_INPUT_SHIFT 8U
#define HDA_GCAP_BIDIRECTIONAL_SHIFT 3U
#define HDA_GCAP_SERIAL_DATA_SHIFT 1U
#define HDA_GCAP_STREAM_MASK UINT32_C(0x0F)
#define HDA_GCAP_BIDIRECTIONAL_MASK UINT32_C(0x1F)
#define HDA_GCAP_SERIAL_DATA_MASK UINT32_C(0x03)

/*
 * Section 7.3.3.1: GET_PARAMETER is a twelve-bit verb, so it sits in bits 19
 * through 8 of the command and its parameter identifier in the low eight.
 */
#define HDA_VERB_GET_PARAMETER UINT32_C(0xF00)
#define HDA_PARAMETER_VENDOR_ID UINT32_C(0x00)
#define HDA_PARAMETER_REVISION_ID UINT32_C(0x02)
#define HDA_PARAMETER_SUBORDINATE_NODES UINT32_C(0x04)
#define HDA_PARAMETER_FUNCTION_GROUP_TYPE UINT32_C(0x05)
#define HDA_FUNCTION_GROUP_AUDIO UINT8_C(0x01)
#define HDA_ROOT_NODE 0U

#define HDA_CODEC_ADDRESS_SHIFT 28U
#define HDA_NODE_SHIFT 20U
#define HDA_VERB_SHIFT 8U

/* Section 7.3.4.11: the subordinate node count packs a start and a count. */
#define HDA_SUBORDINATE_START_SHIFT 16U

/* How many verbs the proof sends per codec: identity, revision, nodes, type. */
#define AUDIO_VERBS_PER_CODEC 4U

struct audio_census {
    struct frame_allocator_stats frames;
    struct paging_state paging;
    struct dma_state dma;
    struct pci_resource_state pci;
    struct interrupt_vector_state vectors;
    struct msix_state msix;
    bool interrupts_enabled;
};

struct audio_controller {
    struct pci_device_claim claim;
    struct pci_mmio_region *region;
    volatile uint8_t *registers;
    struct dma_allocation command_ring;
    struct dma_allocation response_ring;
    volatile uint32_t *commands;
    volatile uint32_t *responses;
    uint16_t command_entries;
    uint16_t response_entries;
    uint16_t command_write;
    uint16_t response_read;
    bool mapped;
    bool bus_master;
    bool rings_running;
    bool controller_running;
};

static struct audio_proof_result installed_result;
static bool audio_active;

static void zero_bytes(void *pointer, size_t length)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static uint8_t mmio_read8(volatile uint8_t *base, uint64_t offset)
{
    return *(volatile uint8_t *)(void *)(base + offset);
}

static uint16_t mmio_read16(volatile uint8_t *base, uint64_t offset)
{
    return *(volatile uint16_t *)(void *)(base + offset);
}

static uint32_t mmio_read32(volatile uint8_t *base, uint64_t offset)
{
    return *(volatile uint32_t *)(void *)(base + offset);
}

static void mmio_write8(volatile uint8_t *base, uint64_t offset, uint8_t value)
{
    *(volatile uint8_t *)(void *)(base + offset) = value;
}

static void mmio_write16(
    volatile uint8_t *base,
    uint64_t offset,
    uint16_t value
)
{
    *(volatile uint16_t *)(void *)(base + offset) = value;
}

static void mmio_write32(
    volatile uint8_t *base,
    uint64_t offset,
    uint32_t value
)
{
    *(volatile uint32_t *)(void *)(base + offset) = value;
}

static bool deadline_reached(uint64_t now, uint64_t deadline)
{
    return now >= deadline;
}

static bool wait_gctl(volatile uint8_t *base, uint32_t expected)
{
    const uint64_t start = clock_monotonic_ns();
    const uint64_t deadline = start + AUDIO_TIMEOUT_NS;

    if (deadline < start) {
        return false;
    }
    while ((mmio_read32(base, HDA_GCTL) & HDA_GCTL_CONTROLLER_RESET) !=
            expected) {
        if (deadline_reached(clock_monotonic_ns(), deadline)) {
            return false;
        }
        __asm__ volatile ("" : : : "memory");
    }
    return true;
}

static void capture_census(struct audio_census *census)
{
    census->frames = frame_allocator_get_stats();
    census->paging = paging_get_state();
    census->dma = dma_get_state();
    census->pci = pci_resource_get_state();
    census->vectors = interrupt_vector_get_state();
    census->msix = msix_get_state();
    census->interrupts_enabled = cpu_interrupts_enabled();
}

static bool census_equal(
    const struct audio_census *left,
    const struct audio_census *right
)
{
    return left->frames.free_frames == right->frames.free_frames &&
        left->frames.allocated_frames == right->frames.allocated_frames &&
        left->paging.table_frames == right->paging.table_frames &&
        left->paging.root_physical_address ==
            right->paging.root_physical_address &&
        left->dma.active_allocations == right->dma.active_allocations &&
        left->dma.cpu_owned_allocations == right->dma.cpu_owned_allocations &&
        left->dma.device_owned_allocations ==
            right->dma.device_owned_allocations &&
        left->pci.active_claims == right->pci.active_claims &&
        left->pci.active_mappings == right->pci.active_mappings &&
        left->pci.mapped_pages == right->pci.mapped_pages &&
        left->pci.bus_masters == right->pci.bus_masters &&
        left->vectors.allocated == right->vectors.allocated &&
        left->msix.active_bindings == right->msix.active_bindings &&
        left->interrupts_enabled == right->interrupts_enabled;
}

static const struct pci_function *discover_controller(void)
{
    for (size_t index = 0U; index < pci_function_count(); ++index) {
        const struct pci_function *function = pci_function_at(index);

        if (function != NULL && function->vendor_id == AUDIO_PCI_VENDOR &&
            function->device_id == AUDIO_PCI_DEVICE &&
            function->class_code == AUDIO_PCI_CLASS &&
            function->subclass == AUDIO_PCI_SUBCLASS) {
            return function;
        }
    }
    return NULL;
}

/*
 * The largest ring the controller says it supports, and the encoding that
 * selects it. A controller that supports nothing is refused rather than
 * assumed to mean the smallest.
 */
static bool select_ring_size(
    uint8_t size_register,
    uint16_t *entries,
    uint8_t *selection
)
{
    const uint8_t capability = (uint8_t)(size_register >>
        HDA_RING_SIZE_CAPABILITY_SHIFT);

    if ((capability & HDA_RING_SIZE_SUPPORTS_256) != 0U) {
        *entries = 256U;
        *selection = 2U;
        return true;
    }
    if ((capability & HDA_RING_SIZE_SUPPORTS_16) != 0U) {
        *entries = 16U;
        *selection = 1U;
        return true;
    }
    if ((capability & HDA_RING_SIZE_SUPPORTS_2) != 0U) {
        *entries = 2U;
        *selection = 0U;
        return true;
    }
    return false;
}

/*
 * Section 3.3.21: the command read pointer is reset by setting its reset bit,
 * observing the controller acknowledge it, clearing it, and observing that.
 * Both halves are waited on, because a controller that never acknowledges is a
 * controller whose ring position is unknown.
 */
static bool reset_command_read_pointer(volatile uint8_t *base)
{
    uint64_t deadline = clock_monotonic_ns() + AUDIO_TIMEOUT_NS;

    mmio_write16(base, HDA_CORBRP, HDA_CORBRP_RESET);
    while ((mmio_read16(base, HDA_CORBRP) & HDA_CORBRP_RESET) == 0U) {
        if (deadline_reached(clock_monotonic_ns(), deadline)) {
            return false;
        }
        __asm__ volatile ("" : : : "memory");
    }
    mmio_write16(base, HDA_CORBRP, 0U);
    deadline = clock_monotonic_ns() + AUDIO_TIMEOUT_NS;
    while ((mmio_read16(base, HDA_CORBRP) & HDA_CORBRP_RESET) != 0U) {
        if (deadline_reached(clock_monotonic_ns(), deadline)) {
            return false;
        }
        __asm__ volatile ("" : : : "memory");
    }
    return true;
}

static uint32_t build_verb(uint8_t codec, uint8_t node, uint32_t parameter)
{
    return ((uint32_t)codec << HDA_CODEC_ADDRESS_SHIFT) |
        ((uint32_t)node << HDA_NODE_SHIFT) |
        (HDA_VERB_GET_PARAMETER << HDA_VERB_SHIFT) | parameter;
}

/*
 * One command, one answer. The write pointer is advanced only after the ring
 * entry itself is visible, and the answer is taken from the entry the
 * controller's own write pointer names - never from a position this side
 * guessed.
 */
static enum audio_status issue_verb(
    struct audio_controller *controller,
    uint8_t codec,
    uint8_t node,
    uint32_t parameter,
    uint32_t *response
)
{
    const uint16_t next = (uint16_t)((controller->command_write + 1U) %
        controller->command_entries);
    uint64_t deadline;
    uint16_t observed;

    if (response == NULL || controller->command_entries == 0U ||
        controller->response_entries == 0U) {
        return AUDIO_STATUS_NULL_ARGUMENT;
    }
    controller->commands[next] = build_verb(codec, node, parameter);
    cpu_store_fence();
    controller->command_write = next;
    mmio_write16(controller->registers, HDA_CORBWP, next);

    deadline = clock_monotonic_ns() + AUDIO_TIMEOUT_NS;
    for (;;) {
        observed = (uint16_t)(mmio_read16(controller->registers,
            HDA_RIRBWP) & (uint16_t)(controller->response_entries - 1U));
        if (observed != controller->response_read) {
            break;
        }
        if (deadline_reached(clock_monotonic_ns(), deadline)) {
            return AUDIO_STATUS_VERB_TIMEOUT;
        }
        __asm__ volatile ("" : : : "memory");
    }
    controller->response_read =
        (uint16_t)((controller->response_read + 1U) %
            controller->response_entries);
    if (controller->response_read != observed) {
        /*
         * The controller answered more than once for one command, which means
         * the ring holds something this driver did not ask for.
         */
        return AUDIO_STATUS_RESPONSE_AUTHENTICATION;
    }
    *response = controller->responses[
        (size_t)controller->response_read * 2U];
    /*
     * Section 4.4.2: the extended half of a response carries the address of
     * the codec that sent it, and marks the ones no command asked for.
     */
    if ((controller->responses[(size_t)controller->response_read * 2U + 1U] &
            UINT32_C(0x1F)) != (uint32_t)codec) {
        return AUDIO_STATUS_RESPONSE_AUTHENTICATION;
    }
    /*
     * An overrun means the controller wrote past what this side had read,
     * which for a polled driver is a lost answer rather than a late one.
     */
    if ((mmio_read8(controller->registers, HDA_RIRBSTS) &
            HDA_RIRBSTS_OVERRUN) != 0U) {
        return AUDIO_STATUS_RESPONSE_AUTHENTICATION;
    }
    mmio_write8(controller->registers, HDA_RIRBSTS,
        HDA_RIRBSTS_ACKNOWLEDGE);
    return AUDIO_STATUS_OK;
}

/*
 * Everything that could be holding the device to memory, undone in the only
 * order that is safe: stop the ring engines, put the controller back in reset
 * so it cannot start them again, withdraw bus mastering, take the rings back
 * from the device, and only then release the memory they were.
 */
static enum audio_status release_controller(
    struct audio_controller *controller,
    struct audio_proof_result *result,
    enum audio_status status
)
{
    bool failed = false;

    if (controller->registers != NULL) {
        if (controller->rings_running) {
            mmio_write8(controller->registers, HDA_RIRBCTL, 0U);
            mmio_write8(controller->registers, HDA_CORBCTL, 0U);
            controller->rings_running = false;
        }
        if (controller->controller_running) {
            mmio_write32(controller->registers, HDA_GCTL, 0U);
            if (!wait_gctl(controller->registers, 0U)) {
                failed = true;
            }
            controller->controller_running = false;
        }
    }
    if (controller->bus_master) {
        if (pci_claim_disable_bus_master(&controller->claim) !=
                PCI_RESOURCE_STATUS_OK) {
            failed = true;
        }
        controller->bus_master = false;
    }
    if (result != NULL) {
        result->bus_master_withdrawn_before_release =
            !controller->bus_master &&
            (controller->command_ring.active ||
                controller->response_ring.active);
    }
    if (controller->response_ring.active &&
        controller->response_ring.owner == DMA_OWNER_DEVICE &&
        dma_transfer_to_cpu(&controller->response_ring) != DMA_STATUS_OK) {
        failed = true;
    }
    if (controller->command_ring.active &&
        controller->command_ring.owner == DMA_OWNER_DEVICE &&
        dma_transfer_to_cpu(&controller->command_ring) != DMA_STATUS_OK) {
        failed = true;
    }
    if (controller->response_ring.active &&
        dma_release(&controller->response_ring) != DMA_STATUS_OK) {
        failed = true;
    }
    if (controller->command_ring.active &&
        dma_release(&controller->command_ring) != DMA_STATUS_OK) {
        failed = true;
    }
    controller->commands = NULL;
    controller->responses = NULL;
    if (controller->mapped &&
        pci_claim_unmap_last_bar(&controller->claim, AUDIO_REGISTER_BAR) !=
            PCI_RESOURCE_STATUS_OK) {
        failed = true;
    }
    controller->mapped = false;
    controller->registers = NULL;
    controller->region = NULL;
    if (controller->claim.active &&
        pci_release_device(&controller->claim) != PCI_RESOURCE_STATUS_OK) {
        failed = true;
    }
    audio_active = false;
    return failed ? AUDIO_STATUS_TEARDOWN : status;
}

static enum audio_status identify_codec(
    struct audio_controller *controller,
    struct audio_proof_result *result,
    uint8_t address,
    struct audio_codec *codec
)
{
    uint32_t identity = 0U;
    uint32_t revision = 0U;
    uint32_t nodes = 0U;
    uint32_t group = 0U;
    enum audio_status status;

    codec->address = address;
    status = issue_verb(controller, address, HDA_ROOT_NODE,
        HDA_PARAMETER_VENDOR_ID, &identity);
    if (status != AUDIO_STATUS_OK) {
        return status;
    }
    ++result->responses_received;
    status = issue_verb(controller, address, HDA_ROOT_NODE,
        HDA_PARAMETER_REVISION_ID, &revision);
    if (status != AUDIO_STATUS_OK) {
        return status;
    }
    ++result->responses_received;
    status = issue_verb(controller, address, HDA_ROOT_NODE,
        HDA_PARAMETER_SUBORDINATE_NODES, &nodes);
    if (status != AUDIO_STATUS_OK) {
        return status;
    }
    ++result->responses_received;
    codec->vendor_device = identity;
    codec->revision = revision;
    codec->first_group_node = (uint8_t)(nodes >> HDA_SUBORDINATE_START_SHIFT);
    codec->group_node_count = (uint8_t)nodes;
    /*
     * Section 7.3.4.1 and 7.3.4.11: a codec names a vendor and a device, and
     * the root node names at least one function group somewhere above it.
     */
    if (identity == 0U || identity == UINT32_C(0xFFFFFFFF) ||
        (identity >> 16U) == 0U || codec->group_node_count == 0U ||
        codec->first_group_node == 0U) {
        return AUDIO_STATUS_IDENTITY;
    }
    status = issue_verb(controller, address, codec->first_group_node,
        HDA_PARAMETER_FUNCTION_GROUP_TYPE, &group);
    if (status != AUDIO_STATUS_OK) {
        return status;
    }
    ++result->responses_received;
    codec->function_group_type = (uint8_t)group;
    codec->audio_function_group =
        codec->function_group_type == HDA_FUNCTION_GROUP_AUDIO;
    codec->identified = true;
    return AUDIO_STATUS_OK;
}

static enum audio_status bring_up(
    struct audio_controller *controller,
    struct audio_proof_result *result,
    const struct pci_function *function
)
{
    struct pci_bus_master_request bus_master;
    struct dma_request request;
    volatile void *pointer = NULL;
    uint64_t command_physical;
    uint64_t response_physical;
    uint16_t present;
    uint8_t command_selection = 0U;
    uint8_t response_selection = 0U;

    if (pci_claim_device(function, &controller->claim) !=
            PCI_RESOURCE_STATUS_OK) {
        return AUDIO_STATUS_CLAIM_FAILURE;
    }
    if (pci_claim_map_bar(&controller->claim, AUDIO_REGISTER_BAR,
            &controller->region) != PCI_RESOURCE_STATUS_OK ||
        controller->region == NULL) {
        return AUDIO_STATUS_MAPPING_FAILURE;
    }
    controller->mapped = true;
    if (controller->region->size < AUDIO_MINIMUM_REGISTER_BYTES ||
        pci_mmio_subregion(controller->region, 0U, controller->region->size,
            &pointer) != PCI_RESOURCE_STATUS_OK || pointer == NULL) {
        return AUDIO_STATUS_REGISTER_WINDOW;
    }
    controller->registers = (volatile uint8_t *)pointer;

    /* Section 4.2.2: out of reset, then in again, then wait for the codecs. */
    mmio_write32(controller->registers, HDA_GCTL, 0U);
    if (!wait_gctl(controller->registers, 0U)) {
        return AUDIO_STATUS_RESET_TIMEOUT;
    }
    mmio_write32(controller->registers, HDA_GCTL,
        HDA_GCTL_CONTROLLER_RESET);
    if (!wait_gctl(controller->registers, HDA_GCTL_CONTROLLER_RESET)) {
        return AUDIO_STATUS_RESET_TIMEOUT;
    }
    controller->controller_running = true;
    result->controller_reset = true;

    result->capability = mmio_read16(controller->registers, HDA_GCAP);
    result->version = ((uint32_t)mmio_read8(controller->registers,
        HDA_VMAJ) << 8U) | mmio_read8(controller->registers, HDA_VMIN);
    result->output_streams = (result->capability >> HDA_GCAP_OUTPUT_SHIFT) &
        HDA_GCAP_STREAM_MASK;
    result->input_streams = (result->capability >> HDA_GCAP_INPUT_SHIFT) &
        HDA_GCAP_STREAM_MASK;
    result->bidirectional_streams =
        (result->capability >> HDA_GCAP_BIDIRECTIONAL_SHIFT) &
            HDA_GCAP_BIDIRECTIONAL_MASK;
    result->serial_data_out_signals =
        (result->capability >> HDA_GCAP_SERIAL_DATA_SHIFT) &
            HDA_GCAP_SERIAL_DATA_MASK;
    if ((result->version >> 8U) != 1U ||
        result->output_streams + result->input_streams == 0U) {
        return AUDIO_STATUS_VERSION;
    }

    {
        const uint64_t deadline = clock_monotonic_ns() + AUDIO_TIMEOUT_NS;

        for (;;) {
            present = mmio_read16(controller->registers, HDA_STATESTS);
            if (present != 0U) {
                break;
            }
            if (deadline_reached(clock_monotonic_ns(), deadline)) {
                return AUDIO_STATUS_CODEC_ABSENT;
            }
            __asm__ volatile ("" : : : "memory");
        }
    }

    /* Both engines stopped before either base register is written. */
    mmio_write8(controller->registers, HDA_CORBCTL, 0U);
    mmio_write8(controller->registers, HDA_RIRBCTL, 0U);

    if (!select_ring_size(mmio_read8(controller->registers, HDA_CORBSIZE),
            &controller->command_entries, &command_selection) ||
        !select_ring_size(mmio_read8(controller->registers, HDA_RIRBSIZE),
            &controller->response_entries, &response_selection) ||
        controller->command_entries > AUDIO_CORB_MAX_ENTRIES ||
        controller->response_entries > AUDIO_RIRB_MAX_ENTRIES) {
        return AUDIO_STATUS_RING_SIZE;
    }

    request.page_count = 1U;
    request.alignment = PAGING_PAGE_SIZE;
    request.maximum_physical_address = UINT64_C(0xFFFFFFFF);
    if (dma_allocate(&request, &controller->command_ring) != DMA_STATUS_OK ||
        dma_allocate(&request, &controller->response_ring) != DMA_STATUS_OK) {
        return AUDIO_STATUS_DMA_FAILURE;
    }
    controller->commands = controller->command_ring.cpu_address;
    controller->responses = controller->response_ring.cpu_address;
    if (controller->commands == NULL || controller->responses == NULL ||
        (uint64_t)controller->command_entries * AUDIO_CORB_ENTRY_BYTES >
            controller->command_ring.byte_length ||
        (uint64_t)controller->response_entries * AUDIO_RIRB_ENTRY_BYTES >
            controller->response_ring.byte_length) {
        return AUDIO_STATUS_DMA_FAILURE;
    }
    for (uint64_t index = 0U;
         index < controller->command_ring.byte_length / 4U; ++index) {
        controller->commands[index] = 0U;
    }
    for (uint64_t index = 0U;
         index < controller->response_ring.byte_length / 4U; ++index) {
        controller->responses[index] = 0U;
    }

    command_physical =
        (uint64_t)controller->command_ring.frames.physical_base;
    response_physical =
        (uint64_t)controller->response_ring.frames.physical_base;
    mmio_write32(controller->registers, HDA_CORBLBASE,
        (uint32_t)command_physical);
    mmio_write32(controller->registers, HDA_CORBUBASE,
        (uint32_t)(command_physical >> 32U));
    mmio_write8(controller->registers, HDA_CORBSIZE,
        (uint8_t)((mmio_read8(controller->registers, HDA_CORBSIZE) &
            (uint8_t)~HDA_RING_SIZE_SELECT_MASK) | command_selection));
    mmio_write32(controller->registers, HDA_RIRBLBASE,
        (uint32_t)response_physical);
    mmio_write32(controller->registers, HDA_RIRBUBASE,
        (uint32_t)(response_physical >> 32U));
    mmio_write8(controller->registers, HDA_RIRBSIZE,
        (uint8_t)((mmio_read8(controller->registers, HDA_RIRBSIZE) &
            (uint8_t)~HDA_RING_SIZE_SELECT_MASK) | response_selection));
    mmio_write16(controller->registers, HDA_RINTCNT,
        HDA_RESPONSE_INTERRUPT_COUNT);

    if (!reset_command_read_pointer(controller->registers)) {
        return AUDIO_STATUS_RING_RESET;
    }
    mmio_write16(controller->registers, HDA_CORBWP, 0U);
    mmio_write16(controller->registers, HDA_RIRBWP, HDA_RIRBWP_RESET);
    controller->command_write = 0U;
    controller->response_read = 0U;

    /*
     * Bus mastering is refused while the rings still belong to this side.
     * Proving that refusal happens is worth as much as the enable that
     * follows it: it is the check that stops a device being let loose on
     * memory nobody declared.
     */
    bus_master.allocations[0] = &controller->command_ring;
    bus_master.allocations[1] = &controller->response_ring;
    bus_master.allocation_count = 2U;
    if (pci_claim_enable_bus_master(&controller->claim, &bus_master) !=
            PCI_RESOURCE_STATUS_DMA_NOT_PREPARED) {
        return AUDIO_STATUS_BUS_MASTER_GUARD_FAILURE;
    }
    if (dma_mark_initialized(&controller->command_ring) != DMA_STATUS_OK ||
        dma_mark_initialized(&controller->response_ring) != DMA_STATUS_OK ||
        dma_transfer_to_device(&controller->command_ring) != DMA_STATUS_OK ||
        dma_transfer_to_device(&controller->response_ring) != DMA_STATUS_OK) {
        return AUDIO_STATUS_DMA_FAILURE;
    }
    if (pci_claim_enable_bus_master(&controller->claim, &bus_master) !=
            PCI_RESOURCE_STATUS_OK) {
        return AUDIO_STATUS_BUS_MASTER_FAILURE;
    }
    controller->bus_master = true;

    cpu_store_fence();
    mmio_write8(controller->registers, HDA_CORBCTL,
        HDA_CORBCTL_DMA_ENABLE);
    mmio_write8(controller->registers, HDA_RIRBCTL,
        HDA_RIRBCTL_DMA_ENABLE);
    if ((mmio_read8(controller->registers, HDA_CORBCTL) &
            HDA_CORBCTL_DMA_ENABLE) == 0U ||
        (mmio_read8(controller->registers, HDA_RIRBCTL) &
            HDA_RIRBCTL_DMA_ENABLE) == 0U) {
        return AUDIO_STATUS_RING_START;
    }
    controller->rings_running = true;
    result->rings_running = true;
    result->corb_entries = controller->command_entries;
    result->rirb_entries = controller->response_entries;

    for (uint8_t address = 0U; address < AUDIO_MAX_CODECS; ++address) {
        if ((present & (uint16_t)(1U << address)) == 0U) {
            continue;
        }
        ++result->codecs_present;
    }
    if (result->codecs_present == 0U) {
        return AUDIO_STATUS_CODEC_ABSENT;
    }
    for (uint8_t address = 0U; address < AUDIO_MAX_CODECS; ++address) {
        enum audio_status status;

        if ((present & (uint16_t)(1U << address)) == 0U) {
            continue;
        }
        result->verbs_issued += AUDIO_VERBS_PER_CODEC;
        status = identify_codec(controller, result,
            address, &result->codecs[address]);
        if (status != AUDIO_STATUS_OK) {
            return status;
        }
        ++result->codecs_identified;
        if (result->codecs[address].audio_function_group) {
            result->audio_function_group_found = true;
        }
    }
    result->device_wrote_response_ring = result->responses_received ==
        result->verbs_issued && result->responses_received != 0U;
    if (!result->device_wrote_response_ring ||
        !result->audio_function_group_found) {
        return AUDIO_STATUS_IDENTITY;
    }
    return AUDIO_STATUS_OK;
}

bool audio_foundation_self_test(size_t *completed_tests)
{
    struct audio_controller probe;
    uint16_t entries = 0U;
    uint8_t selection = 0xFFU;
    size_t completed = 0U;

    if (completed_tests == NULL) {
        return false;
    }
    *completed_tests = 0U;

    /* A verb is exactly the command word the specification describes. */
    if (build_verb(0U, HDA_ROOT_NODE, HDA_PARAMETER_VENDOR_ID) !=
            UINT32_C(0x000F0000) ||
        build_verb(1U, HDA_ROOT_NODE, HDA_PARAMETER_REVISION_ID) !=
            UINT32_C(0x100F0002) ||
        build_verb(2U, 1U, HDA_PARAMETER_FUNCTION_GROUP_TYPE) !=
            UINT32_C(0x201F0005)) {
        return false;
    }
    ++completed;
    /* The codec address and node fields do not overlap the verb. */
    if ((build_verb(AUDIO_MAX_CODECS, 0x7FU, 0U) & UINT32_C(0x000FFFFF)) !=
            (HDA_VERB_GET_PARAMETER << HDA_VERB_SHIFT)) {
        return false;
    }
    ++completed;
    /* The largest supported ring is taken, and only a supported one. */
    if (!select_ring_size(UINT8_C(0x40), &entries, &selection) ||
        entries != 256U || selection != 2U) {
        return false;
    }
    ++completed;
    if (!select_ring_size(UINT8_C(0x20), &entries, &selection) ||
        entries != 16U || selection != 1U) {
        return false;
    }
    ++completed;
    if (!select_ring_size(UINT8_C(0x10), &entries, &selection) ||
        entries != 2U || selection != 0U) {
        return false;
    }
    ++completed;
    if (select_ring_size(UINT8_C(0x00), &entries, &selection) ||
        select_ring_size(UINT8_C(0x0F), &entries, &selection)) {
        return false;
    }
    ++completed;
    /* Both rings fit the one page each is allocated from. */
    if (AUDIO_CORB_MAX_ENTRIES * AUDIO_CORB_ENTRY_BYTES > PAGING_PAGE_SIZE ||
        AUDIO_RIRB_MAX_ENTRIES * AUDIO_RIRB_ENTRY_BYTES > PAGING_PAGE_SIZE) {
        return false;
    }
    ++completed;
    /* A ring size is a power of two, which is what the wrap depends on. */
    if ((AUDIO_CORB_MAX_ENTRIES & (AUDIO_CORB_MAX_ENTRIES - 1U)) != 0U ||
        (AUDIO_RIRB_MAX_ENTRIES & (AUDIO_RIRB_MAX_ENTRIES - 1U)) != 0U) {
        return false;
    }
    ++completed;
    /* Releasing a controller that was never brought up changes nothing. */
    zero_bytes(&probe, sizeof(probe));
    audio_active = true;
    if (release_controller(&probe, NULL, AUDIO_STATUS_OK) !=
            AUDIO_STATUS_OK || audio_active) {
        return false;
    }
    ++completed;
    /* Every status has a message and the table is complete. */
    for (int status = 0; status < (int)AUDIO_STATUS_COUNT; ++status) {
        const char *message = audio_status_string((enum audio_status)status);

        if (message == NULL || message[0] == '\0') {
            return false;
        }
    }
    if (audio_status_string(AUDIO_STATUS_COUNT)[0] != 'u' ||
        !audio_resources_released()) {
        return false;
    }
    ++completed;
    *completed_tests = completed;
    return completed == AUDIO_CONTROLLED_CONTROLS;
}

enum audio_status audio_prove(struct audio_proof_result *result)
{
    struct audio_controller controller;
    struct audio_census before;
    struct audio_census after;
    const struct pci_function *function;
    const bool restore_interrupts = cpu_interrupts_enabled();
    enum audio_status status;
    size_t completed = 0U;

    if (result == NULL) {
        return AUDIO_STATUS_NULL_ARGUMENT;
    }
    zero_bytes(result, sizeof(*result));
    if (audio_active) {
        return AUDIO_STATUS_BUSY;
    }
    if (!pci_is_initialized() || !pci_resource_get_state().active ||
        !dma_get_state().active) {
        return AUDIO_STATUS_PREREQUISITE;
    }
    if (!audio_foundation_self_test(&completed) ||
        completed != AUDIO_CONTROLLED_CONTROLS) {
        return AUDIO_STATUS_PREREQUISITE;
    }
    result->controls = (uint32_t)completed;
    function = discover_controller();
    if (function == NULL) {
        return AUDIO_STATUS_ABSENT;
    }

    cpu_interrupt_disable();
    capture_census(&before);
    zero_bytes(&controller, sizeof(controller));
    audio_active = true;
    status = bring_up(&controller, result, function);
    status = release_controller(&controller, result, status);
    capture_census(&after);
    if (restore_interrupts) {
        cpu_interrupt_enable();
    }
    result->teardown_complete = pci_resource_verify() ==
            PCI_RESOURCE_STATUS_OK && dma_verify() == DMA_STATUS_OK &&
        dma_get_state().active_allocations == 0U &&
        pci_resource_get_state().bus_masters == 0U;
    result->resource_census_equal = census_equal(&before, &after);
    if (status != AUDIO_STATUS_OK) {
        zero_bytes(result, sizeof(*result));
        return status;
    }
    if (!result->teardown_complete) {
        zero_bytes(result, sizeof(*result));
        return AUDIO_STATUS_TEARDOWN;
    }
    if (!result->resource_census_equal) {
        zero_bytes(result, sizeof(*result));
        return AUDIO_STATUS_RESOURCE_CENSUS;
    }
    installed_result = *result;
    return AUDIO_STATUS_OK;
}

struct audio_proof_result audio_get_proof_result(void)
{
    return installed_result;
}

bool audio_resources_released(void)
{
    return !audio_active;
}

const char *audio_status_string(enum audio_status status)
{
    static const char *const messages[AUDIO_STATUS_COUNT] = {
        "ok",
        "null audio argument",
        "the bounded audio proof is already active",
        "audio prerequisites are incomplete",
        "no HD Audio controller is present",
        "audio controller claim failed",
        "audio register window mapping failed",
        "the mapped audio register window is too small",
        "the controller does not report a known interface version",
        "an audio controller reset did not complete inside its bound",
        "audio ring DMA allocation or ownership failed",
        "bus mastering was granted before the rings were prepared",
        "enabling bus mastering for the audio rings failed",
        "the controller supports no usable ring size",
        "the command ring read pointer refused to reset",
        "an audio ring DMA engine refused to start",
        "the controller reports no codec on the link",
        "a codec did not answer inside its bound",
        "a response did not come from the codec that was asked",
        "a codec did not identify itself as its specification requires",
        "audio teardown leaked or failed",
        "audio pre/post resource census differs"
    };

    _Static_assert(sizeof(messages) / sizeof(messages[0]) ==
        AUDIO_STATUS_COUNT, "audio status messages are out of sync");
    if (status < AUDIO_STATUS_OK || status >= AUDIO_STATUS_COUNT) {
        return "unknown audio status";
    }
    return messages[status];
}
