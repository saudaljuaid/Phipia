/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zenith/acpi.h>

/* ACPI 6.6 sections 5.2.12.2, 5.2.12.3, 5.2.12.5, 5.2.12.8,
 * and 5.2.12.12 define these record types and exact wire lengths. */
#define MADT_FIXED_SIZE 44U
#define MADT_ENTRY_HEADER_SIZE 2U
#define MADT_LOCAL_APIC_TYPE UINT8_C(0)
#define MADT_IO_APIC_TYPE UINT8_C(1)
#define MADT_INTERRUPT_OVERRIDE_TYPE UINT8_C(2)
#define MADT_LOCAL_APIC_OVERRIDE_TYPE UINT8_C(5)
#define MADT_LOCAL_X2APIC_TYPE UINT8_C(9)
#define MADT_LOCAL_APIC_SIZE 8U
#define MADT_IO_APIC_SIZE 12U
#define MADT_INTERRUPT_OVERRIDE_SIZE 10U
#define MADT_LOCAL_APIC_OVERRIDE_SIZE 12U
#define MADT_LOCAL_X2APIC_SIZE 16U
#define MADT_MAX_TABLE_SIZE (1024U * 1024U)
#define MADT_PCAT_COMPAT UINT32_C(1)
#define ISA_INTERRUPT_COUNT 16U
#define MPS_POLARITY_MASK UINT16_C(0x0003)
#define MPS_TRIGGER_MASK UINT16_C(0x000C)
#define MPS_TRIGGER_SHIFT 2U
#define MPS_RESERVED_MASK UINT16_C(0xFFF0)
#define MPS_RESERVED_ENCODING UINT16_C(2)

static const char madt_signature[4] = {'A', 'P', 'I', 'C'};

static uint16_t read_u16(const uint8_t *bytes)
{
    uint16_t value = bytes[0];

    value |= (uint16_t)bytes[1] << 8U;
    return value;
}

static uint32_t read_u32(const uint8_t *bytes)
{
    uint32_t value = bytes[0];

    value |= (uint32_t)bytes[1] << 8U;
    value |= (uint32_t)bytes[2] << 16U;
    value |= (uint32_t)bytes[3] << 24U;
    return value;
}

static uint64_t read_u64(const uint8_t *bytes)
{
    uint64_t value = read_u32(bytes);

    value |= (uint64_t)read_u32(bytes + sizeof(uint32_t)) << 32U;
    return value;
}

static bool bytes_equal(const uint8_t *left, const char *right, size_t size)
{
    for (size_t index = 0; index < size; ++index) {
        if (left[index] != (uint8_t)right[index]) {
            return false;
        }
    }

    return true;
}

static uint8_t byte_sum(const uint8_t *bytes, size_t size)
{
    uint8_t sum = 0U;

    for (size_t index = 0; index < size; ++index) {
        sum = (uint8_t)(sum + bytes[index]);
    }

    return sum;
}

static bool physical_span_is_mapped(uint64_t address, uint64_t length)
{
    return address < ZENITH_EARLY_PHYSICAL_LIMIT &&
        length <= ZENITH_EARLY_PHYSICAL_LIMIT - address;
}

static void topology_reset(struct acpi_topology *topology)
{
    uint8_t *bytes = (uint8_t *)(void *)topology;

    for (size_t index = 0; index < sizeof(*topology); ++index) {
        bytes[index] = 0U;
    }
}

static bool processor_flags_valid(uint32_t flags)
{
    const uint32_t supported =
        ACPI_PROCESSOR_ENABLED | ACPI_PROCESSOR_ONLINE_CAPABLE;

    return (flags & ~supported) == 0U && flags != supported;
}

static enum acpi_status add_processor(
    struct acpi_topology *topology,
    enum acpi_processor_kind kind,
    uint32_t acpi_uid,
    uint32_t apic_id,
    uint32_t flags
)
{
    struct acpi_processor *processor;

    if (!processor_flags_valid(flags)) {
        return ACPI_STATUS_BAD_PROCESSOR_FLAGS;
    }

    if (flags == 0U) {
        ++topology->ignored_processor_count;
        return ACPI_STATUS_OK;
    }

    if (topology->processor_count == ACPI_MAX_PROCESSORS) {
        return ACPI_STATUS_TOO_MANY_PROCESSORS;
    }

    for (size_t index = 0; index < topology->processor_count; ++index) {
        if (topology->processors[index].acpi_uid == acpi_uid) {
            return ACPI_STATUS_DUPLICATE_PROCESSOR_UID;
        }

        if (topology->processors[index].apic_id == apic_id) {
            return ACPI_STATUS_DUPLICATE_APIC_ID;
        }
    }

    processor = &topology->processors[topology->processor_count];
    processor->kind = kind;
    processor->acpi_uid = acpi_uid;
    processor->apic_id = apic_id;
    processor->flags = flags;
    ++topology->processor_count;

    if ((flags & ACPI_PROCESSOR_ENABLED) != 0U) {
        ++topology->enabled_processor_count;
    } else {
        ++topology->online_capable_processor_count;
    }

    return ACPI_STATUS_OK;
}

static enum acpi_status parse_local_apic(
    const uint8_t *entry,
    struct acpi_topology *topology
)
{
    if (entry[1] != MADT_LOCAL_APIC_SIZE) {
        return ACPI_STATUS_BAD_MADT_ENTRY_LENGTH;
    }

    return add_processor(
        topology,
        ACPI_PROCESSOR_LOCAL_APIC,
        entry[2],
        entry[3],
        read_u32(entry + 4U)
    );
}

static enum acpi_status parse_local_x2apic(
    const uint8_t *entry,
    struct acpi_topology *topology
)
{
    if (entry[1] != MADT_LOCAL_X2APIC_SIZE) {
        return ACPI_STATUS_BAD_MADT_ENTRY_LENGTH;
    }

    if (read_u16(entry + 2U) != 0U) {
        return ACPI_STATUS_BAD_PROCESSOR_RESERVED;
    }

    return add_processor(
        topology,
        ACPI_PROCESSOR_LOCAL_X2APIC,
        read_u32(entry + 12U),
        read_u32(entry + 4U),
        read_u32(entry + 8U)
    );
}

static enum acpi_status parse_io_apic(
    const uint8_t *entry,
    struct acpi_topology *topology
)
{
    struct acpi_io_apic *io_apic;
    uint32_t physical_address;
    uint32_t global_interrupt_base;

    if (entry[1] != MADT_IO_APIC_SIZE) {
        return ACPI_STATUS_BAD_MADT_ENTRY_LENGTH;
    }

    physical_address = read_u32(entry + 4U);
    global_interrupt_base = read_u32(entry + 8U);

    if (entry[3] != 0U) {
        return ACPI_STATUS_BAD_IO_APIC_RESERVED;
    }

    if (physical_address == 0U) {
        return ACPI_STATUS_NULL_IO_APIC_ADDRESS;
    }

    if (topology->io_apic_count == ACPI_MAX_IO_APICS) {
        return ACPI_STATUS_TOO_MANY_IO_APICS;
    }

    for (size_t index = 0; index < topology->io_apic_count; ++index) {
        if (topology->io_apics[index].id == entry[2]) {
            return ACPI_STATUS_DUPLICATE_IO_APIC_ID;
        }

        if (topology->io_apics[index].physical_address == physical_address) {
            return ACPI_STATUS_DUPLICATE_IO_APIC_ADDRESS;
        }

        if (topology->io_apics[index].global_interrupt_base ==
            global_interrupt_base) {
            return ACPI_STATUS_DUPLICATE_GSI_BASE;
        }
    }

    io_apic = &topology->io_apics[topology->io_apic_count];
    io_apic->id = entry[2];
    io_apic->physical_address = physical_address;
    io_apic->global_interrupt_base = global_interrupt_base;
    ++topology->io_apic_count;
    return ACPI_STATUS_OK;
}

static enum acpi_status parse_interrupt_override(
    const uint8_t *entry,
    struct acpi_topology *topology
)
{
    struct acpi_interrupt_override *override;
    uint32_t source;
    uint32_t global_interrupt;
    uint16_t flags;
    uint16_t polarity;
    uint16_t trigger;

    if (entry[1] != MADT_INTERRUPT_OVERRIDE_SIZE) {
        return ACPI_STATUS_BAD_MADT_ENTRY_LENGTH;
    }

    source = entry[3];
    global_interrupt = read_u32(entry + 4U);
    flags = read_u16(entry + 8U);
    polarity = flags & MPS_POLARITY_MASK;
    trigger = (flags & MPS_TRIGGER_MASK) >> MPS_TRIGGER_SHIFT;

    if (entry[2] != 0U) {
        return ACPI_STATUS_BAD_INTERRUPT_OVERRIDE_BUS;
    }

    if (source >= ISA_INTERRUPT_COUNT) {
        return ACPI_STATUS_BAD_INTERRUPT_OVERRIDE_SOURCE;
    }

    if ((flags & MPS_RESERVED_MASK) != 0U ||
        polarity == MPS_RESERVED_ENCODING ||
        trigger == MPS_RESERVED_ENCODING) {
        return ACPI_STATUS_BAD_INTERRUPT_FLAGS;
    }

    if (topology->interrupt_override_count ==
        ACPI_MAX_INTERRUPT_OVERRIDES) {
        return ACPI_STATUS_TOO_MANY_INTERRUPT_OVERRIDES;
    }

    for (size_t index = 0;
         index < topology->interrupt_override_count;
         ++index) {
        if (topology->interrupt_overrides[index].source == source) {
            return ACPI_STATUS_DUPLICATE_INTERRUPT_SOURCE;
        }

        if (topology->interrupt_overrides[index].global_interrupt ==
            global_interrupt) {
            return ACPI_STATUS_DUPLICATE_INTERRUPT_GSI;
        }
    }

    override = &topology->interrupt_overrides[
        topology->interrupt_override_count
    ];
    override->source = source;
    override->global_interrupt = global_interrupt;
    override->polarity = (enum acpi_interrupt_polarity)polarity;
    override->trigger = (enum acpi_interrupt_trigger)trigger;
    ++topology->interrupt_override_count;
    return ACPI_STATUS_OK;
}

static enum acpi_status parse_local_apic_override(
    const uint8_t *entry,
    struct acpi_topology *topology
)
{
    uint64_t address;

    if (entry[1] != MADT_LOCAL_APIC_OVERRIDE_SIZE) {
        return ACPI_STATUS_BAD_MADT_ENTRY_LENGTH;
    }

    if (read_u16(entry + 2U) != 0U) {
        return ACPI_STATUS_BAD_LOCAL_APIC_OVERRIDE_RESERVED;
    }

    if (topology->local_apic_address_overridden) {
        return ACPI_STATUS_DUPLICATE_LOCAL_APIC_OVERRIDE;
    }

    address = read_u64(entry + 4U);

    if (address == 0U) {
        return ACPI_STATUS_NULL_LOCAL_APIC_ADDRESS;
    }

    topology->local_apic_address = address;
    topology->local_apic_address_overridden = true;
    return ACPI_STATUS_OK;
}

static enum acpi_status parse_entry(
    const uint8_t *entry,
    struct acpi_topology *topology
)
{
    switch (entry[0]) {
    case MADT_LOCAL_APIC_TYPE:
        return parse_local_apic(entry, topology);
    case MADT_IO_APIC_TYPE:
        return parse_io_apic(entry, topology);
    case MADT_INTERRUPT_OVERRIDE_TYPE:
        return parse_interrupt_override(entry, topology);
    case MADT_LOCAL_APIC_OVERRIDE_TYPE:
        return parse_local_apic_override(entry, topology);
    case MADT_LOCAL_X2APIC_TYPE:
        return parse_local_x2apic(entry, topology);
    default:
        ++topology->ignored_entry_count;
        return ACPI_STATUS_OK;
    }
}

enum acpi_status acpi_topology_discover(
    const struct acpi_madt *madt,
    struct acpi_topology *topology
)
{
    const uint8_t *bytes;
    uint32_t wire_length;
    size_t offset = MADT_FIXED_SIZE;

    if (madt == NULL || topology == NULL) {
        return ACPI_STATUS_NULL_ARGUMENT;
    }

    topology_reset(topology);

    if (madt->physical_address == 0U) {
        return ACPI_STATUS_NULL_TABLE;
    }

    if (!physical_span_is_mapped(madt->physical_address, MADT_FIXED_SIZE)) {
        return ACPI_STATUS_TABLE_OUTSIDE_EARLY_MAP;
    }

    bytes = (const uint8_t *)(uintptr_t)madt->physical_address;
    wire_length = read_u32(bytes + 4U);

    if (!bytes_equal(bytes, madt_signature, sizeof(madt_signature)) ||
        madt->length < MADT_FIXED_SIZE ||
        madt->length > MADT_MAX_TABLE_SIZE ||
        wire_length != madt->length) {
        return ACPI_STATUS_BAD_MADT_LENGTH;
    }

    if (!physical_span_is_mapped(madt->physical_address, madt->length)) {
        return ACPI_STATUS_TABLE_OUTSIDE_EARLY_MAP;
    }

    if (byte_sum(bytes, madt->length) != 0U) {
        return ACPI_STATUS_BAD_TABLE_CHECKSUM;
    }

    if ((read_u32(bytes + 40U) & ~MADT_PCAT_COMPAT) != 0U) {
        return ACPI_STATUS_BAD_MADT_FLAGS;
    }

    topology->local_apic_address = read_u32(bytes + 36U);

    while (offset < madt->length) {
        const uint8_t *entry;
        size_t entry_length;
        enum acpi_status status;

        if (madt->length - offset < MADT_ENTRY_HEADER_SIZE) {
            topology_reset(topology);
            return ACPI_STATUS_BAD_MADT_ENTRY_HEADER;
        }

        entry = bytes + offset;
        entry_length = entry[1];

        if (entry_length < MADT_ENTRY_HEADER_SIZE ||
            entry_length > madt->length - offset) {
            topology_reset(topology);
            return ACPI_STATUS_BAD_MADT_ENTRY_LENGTH;
        }

        if (topology->madt_entry_count == ACPI_MAX_MADT_ENTRIES) {
            topology_reset(topology);
            return ACPI_STATUS_TOO_MANY_MADT_ENTRIES;
        }

        ++topology->madt_entry_count;
        status = parse_entry(entry, topology);

        if (status != ACPI_STATUS_OK) {
            topology_reset(topology);
            return status;
        }

        offset += entry_length;
    }

    if (topology->processor_count == 0U) {
        topology_reset(topology);
        return ACPI_STATUS_MISSING_USABLE_PROCESSOR;
    }

    if (topology->io_apic_count == 0U) {
        topology_reset(topology);
        return ACPI_STATUS_MISSING_IO_APIC;
    }

    if (topology->local_apic_address == 0U) {
        topology_reset(topology);
        return ACPI_STATUS_NULL_LOCAL_APIC_ADDRESS;
    }

    return ACPI_STATUS_OK;
}
