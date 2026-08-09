/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zenith/acpi.h>

#define TEST_MADT_CAPACITY 8192U
#define TEST_MADT_FIXED_SIZE 44U
#define TEST_LOCAL_APIC_SIZE 8U
#define TEST_IO_APIC_SIZE 12U
#define TEST_INTERRUPT_OVERRIDE_SIZE 10U
#define TEST_LOCAL_APIC_OVERRIDE_SIZE 12U
#define TEST_LOCAL_X2APIC_SIZE 16U
#define TEST_LOCAL_APIC_TYPE UINT8_C(0)
#define TEST_IO_APIC_TYPE UINT8_C(1)
#define TEST_INTERRUPT_OVERRIDE_TYPE UINT8_C(2)
#define TEST_LOCAL_APIC_OVERRIDE_TYPE UINT8_C(5)
#define TEST_LOCAL_X2APIC_TYPE UINT8_C(9)
#define TEST_ISA_INTERRUPT_COUNT 16U

struct topology_test_fixture {
    uint8_t bytes[TEST_MADT_CAPACITY];
    size_t length;
};

static struct topology_test_fixture test_fixture __attribute__((aligned(8)));
static struct acpi_topology test_topology;

_Static_assert(
    TEST_MADT_FIXED_SIZE + TEST_IO_APIC_SIZE +
        (ACPI_MAX_PROCESSORS + 1U) * TEST_LOCAL_X2APIC_SIZE <=
        TEST_MADT_CAPACITY,
    "largest topology test fixture exceeds its static buffer"
);

static void write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
}

static void write_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
    bytes[2] = (uint8_t)(value >> 16U);
    bytes[3] = (uint8_t)(value >> 24U);
}

static void write_u64(uint8_t *bytes, uint64_t value)
{
    write_u32(bytes, (uint32_t)value);
    write_u32(bytes + sizeof(uint32_t), (uint32_t)(value >> 32U));
}

static uint8_t byte_sum(const uint8_t *bytes, size_t size)
{
    uint8_t sum = 0U;

    for (size_t index = 0; index < size; ++index) {
        sum = (uint8_t)(sum + bytes[index]);
    }

    return sum;
}

static void fixture_begin(uint32_t local_apic_address)
{
    for (size_t index = 0; index < sizeof(test_fixture.bytes); ++index) {
        test_fixture.bytes[index] = 0U;
    }

    test_fixture.bytes[0] = 'A';
    test_fixture.bytes[1] = 'P';
    test_fixture.bytes[2] = 'I';
    test_fixture.bytes[3] = 'C';
    test_fixture.bytes[8] = 6U;
    test_fixture.bytes[10] = 'Z';
    test_fixture.bytes[11] = 'E';
    test_fixture.bytes[12] = 'N';
    test_fixture.bytes[13] = 'I';
    test_fixture.bytes[14] = 'T';
    test_fixture.bytes[15] = 'H';
    write_u32(test_fixture.bytes + 36U, local_apic_address);
    write_u32(test_fixture.bytes + 40U, UINT32_C(1));
    test_fixture.length = TEST_MADT_FIXED_SIZE;
}

static uint8_t *fixture_append(uint8_t type, uint8_t length)
{
    uint8_t *entry = test_fixture.bytes + test_fixture.length;

    entry[0] = type;
    entry[1] = length;

    for (size_t index = 2U; index < length; ++index) {
        entry[index] = 0U;
    }

    test_fixture.length += length;
    return entry;
}

static void fixture_add_local_apic(
    uint8_t uid,
    uint8_t apic_id,
    uint32_t flags
)
{
    uint8_t *entry = fixture_append(
        TEST_LOCAL_APIC_TYPE,
        TEST_LOCAL_APIC_SIZE
    );

    entry[2] = uid;
    entry[3] = apic_id;
    write_u32(entry + 4U, flags);
}

static void fixture_add_local_x2apic(
    uint32_t uid,
    uint32_t apic_id,
    uint32_t flags,
    uint16_t reserved
)
{
    uint8_t *entry = fixture_append(
        TEST_LOCAL_X2APIC_TYPE,
        TEST_LOCAL_X2APIC_SIZE
    );

    write_u16(entry + 2U, reserved);
    write_u32(entry + 4U, apic_id);
    write_u32(entry + 8U, flags);
    write_u32(entry + 12U, uid);
}

static void fixture_add_io_apic(
    uint8_t id,
    uint8_t reserved,
    uint32_t address,
    uint32_t global_interrupt_base
)
{
    uint8_t *entry = fixture_append(
        TEST_IO_APIC_TYPE,
        TEST_IO_APIC_SIZE
    );

    entry[2] = id;
    entry[3] = reserved;
    write_u32(entry + 4U, address);
    write_u32(entry + 8U, global_interrupt_base);
}

static void fixture_add_interrupt_override(
    uint8_t bus,
    uint8_t source,
    uint32_t global_interrupt,
    uint16_t flags
)
{
    uint8_t *entry = fixture_append(
        TEST_INTERRUPT_OVERRIDE_TYPE,
        TEST_INTERRUPT_OVERRIDE_SIZE
    );

    entry[2] = bus;
    entry[3] = source;
    write_u32(entry + 4U, global_interrupt);
    write_u16(entry + 8U, flags);
}

static void fixture_add_local_apic_override(
    uint16_t reserved,
    uint64_t address
)
{
    uint8_t *entry = fixture_append(
        TEST_LOCAL_APIC_OVERRIDE_TYPE,
        TEST_LOCAL_APIC_OVERRIDE_SIZE
    );

    write_u16(entry + 2U, reserved);
    write_u64(entry + 4U, address);
}

static void fixture_add_unknown(void)
{
    (void)fixture_append(UINT8_C(0x7F), UINT8_C(2));
}

static void fixture_add_minimum(void)
{
    fixture_add_local_apic(0U, 0U, ACPI_PROCESSOR_ENABLED);
    fixture_add_io_apic(1U, 0U, UINT32_C(0xFEC00000), 0U);
}

static struct acpi_madt fixture_finish(void)
{
    struct acpi_madt madt = {
        .physical_address =
            (uint64_t)(uintptr_t)(void *)test_fixture.bytes,
        .length = (uint32_t)test_fixture.length,
        .local_apic_address = 0U,
        .flags = 0U,
        .root_entry_count = 1U,
        .revision = 6U,
        .oem_id = {'Z', 'E', 'N', 'I', 'T', 'H', '\0'},
        .oem_table_id = {'Z', 'T', 'T', 'O', 'P', 'O', 'L', 'O', '\0'}
    };

    write_u32(test_fixture.bytes + 4U, (uint32_t)test_fixture.length);
    test_fixture.bytes[9] = 0U;
    test_fixture.bytes[9] = (uint8_t)(
        0U - byte_sum(test_fixture.bytes, test_fixture.length)
    );
    madt.local_apic_address =
        (uint32_t)test_fixture.bytes[36] |
        (uint32_t)test_fixture.bytes[37] << 8U |
        (uint32_t)test_fixture.bytes[38] << 16U |
        (uint32_t)test_fixture.bytes[39] << 24U;
    madt.flags =
        (uint32_t)test_fixture.bytes[40] |
        (uint32_t)test_fixture.bytes[41] << 8U |
        (uint32_t)test_fixture.bytes[42] << 16U |
        (uint32_t)test_fixture.bytes[43] << 24U;
    return madt;
}

static enum acpi_status fixture_status(void)
{
    struct acpi_madt madt = fixture_finish();

    return acpi_topology_discover(&madt, &test_topology);
}

static bool valid_fixture_is_accepted(void)
{
    const uint64_t override_address = UINT64_C(0x00000000FEE01000);

    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_local_apic(0U, 0U, ACPI_PROCESSOR_ENABLED);
    fixture_add_local_apic(1U, 1U, 0U);
    fixture_add_local_x2apic(
        UINT32_C(300),
        UINT32_C(0x100),
        ACPI_PROCESSOR_ONLINE_CAPABLE,
        0U
    );
    fixture_add_io_apic(2U, 0U, UINT32_C(0xFEC00000), 0U);
    fixture_add_interrupt_override(0U, 0U, 2U, UINT16_C(0x000F));
    fixture_add_local_apic_override(0U, override_address);
    fixture_add_unknown();

    return fixture_status() == ACPI_STATUS_OK &&
        test_topology.local_apic_address == override_address &&
        test_topology.local_apic_address_overridden &&
        test_topology.madt_entry_count == 7U &&
        test_topology.ignored_entry_count == 1U &&
        test_topology.ignored_processor_count == 1U &&
        test_topology.processor_count == 2U &&
        test_topology.enabled_processor_count == 1U &&
        test_topology.online_capable_processor_count == 1U &&
        test_topology.processors[0].kind == ACPI_PROCESSOR_LOCAL_APIC &&
        test_topology.processors[1].kind == ACPI_PROCESSOR_LOCAL_X2APIC &&
        test_topology.processors[1].acpi_uid == UINT32_C(300) &&
        test_topology.processors[1].apic_id == UINT32_C(0x100) &&
        test_topology.io_apic_count == 1U &&
        test_topology.io_apics[0].id == 2U &&
        test_topology.interrupt_override_count == 1U &&
        test_topology.interrupt_overrides[0].source == 0U &&
        test_topology.interrupt_overrides[0].global_interrupt == 2U &&
        test_topology.interrupt_overrides[0].polarity ==
            ACPI_POLARITY_ACTIVE_LOW &&
        test_topology.interrupt_overrides[0].trigger ==
            ACPI_TRIGGER_LEVEL;
}

static bool entry_bounds_are_rejected(void)
{
    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_minimum();
    test_fixture.bytes[test_fixture.length] = UINT8_C(0x7F);
    ++test_fixture.length;

    if (fixture_status() != ACPI_STATUS_BAD_MADT_ENTRY_HEADER) {
        return false;
    }

    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_minimum();
    (void)fixture_append(UINT8_C(0x7F), UINT8_C(2));
    test_fixture.bytes[test_fixture.length - 1U] = 0U;

    if (fixture_status() != ACPI_STATUS_BAD_MADT_ENTRY_LENGTH) {
        return false;
    }

    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_minimum();
    test_fixture.bytes[test_fixture.length] = UINT8_C(0x7F);
    test_fixture.bytes[test_fixture.length + 1U] = UINT8_C(4);
    test_fixture.length += 2U;

    if (fixture_status() != ACPI_STATUS_BAD_MADT_ENTRY_LENGTH) {
        return false;
    }

    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_minimum();
    (void)fixture_append(TEST_LOCAL_APIC_TYPE, UINT8_C(7));

    if (fixture_status() != ACPI_STATUS_BAD_MADT_ENTRY_LENGTH) {
        return false;
    }

    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_minimum();

    for (size_t index = 0; index < ACPI_MAX_MADT_ENTRIES - 1U; ++index) {
        fixture_add_unknown();
    }

    return fixture_status() == ACPI_STATUS_TOO_MANY_MADT_ENTRIES;
}

static bool processor_errors_are_rejected(void)
{
    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_minimum();
    fixture_add_local_apic(1U, 1U, UINT32_C(4));

    if (fixture_status() != ACPI_STATUS_BAD_PROCESSOR_FLAGS) {
        return false;
    }

    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_minimum();
    fixture_add_local_apic(
        1U,
        1U,
        ACPI_PROCESSOR_ENABLED | ACPI_PROCESSOR_ONLINE_CAPABLE
    );

    if (fixture_status() != ACPI_STATUS_BAD_PROCESSOR_FLAGS) {
        return false;
    }

    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_minimum();
    fixture_add_local_x2apic(1U, 1U, ACPI_PROCESSOR_ENABLED, 1U);

    if (fixture_status() != ACPI_STATUS_BAD_PROCESSOR_RESERVED) {
        return false;
    }

    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_minimum();
    fixture_add_local_x2apic(0U, 1U, ACPI_PROCESSOR_ENABLED, 0U);

    if (fixture_status() != ACPI_STATUS_DUPLICATE_PROCESSOR_UID) {
        return false;
    }

    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_minimum();
    fixture_add_local_x2apic(1U, 0U, ACPI_PROCESSOR_ENABLED, 0U);

    if (fixture_status() != ACPI_STATUS_DUPLICATE_APIC_ID) {
        return false;
    }

    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_io_apic(1U, 0U, UINT32_C(0xFEC00000), 0U);

    for (size_t index = 0; index <= ACPI_MAX_PROCESSORS; ++index) {
        fixture_add_local_x2apic(
            (uint32_t)index,
            (uint32_t)index,
            ACPI_PROCESSOR_ENABLED,
            0U
        );
    }

    return fixture_status() == ACPI_STATUS_TOO_MANY_PROCESSORS;
}

static bool io_apic_errors_are_rejected(void)
{
    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_local_apic(0U, 0U, ACPI_PROCESSOR_ENABLED);
    fixture_add_io_apic(1U, 1U, UINT32_C(0xFEC00000), 0U);

    if (fixture_status() != ACPI_STATUS_BAD_IO_APIC_RESERVED) {
        return false;
    }

    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_local_apic(0U, 0U, ACPI_PROCESSOR_ENABLED);
    fixture_add_io_apic(1U, 0U, 0U, 0U);

    if (fixture_status() != ACPI_STATUS_NULL_IO_APIC_ADDRESS) {
        return false;
    }

    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_minimum();
    fixture_add_io_apic(1U, 0U, UINT32_C(0xFEC01000), 24U);

    if (fixture_status() != ACPI_STATUS_DUPLICATE_IO_APIC_ID) {
        return false;
    }

    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_minimum();
    fixture_add_io_apic(2U, 0U, UINT32_C(0xFEC00000), 24U);

    if (fixture_status() != ACPI_STATUS_DUPLICATE_IO_APIC_ADDRESS) {
        return false;
    }

    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_minimum();
    fixture_add_io_apic(2U, 0U, UINT32_C(0xFEC01000), 0U);

    if (fixture_status() != ACPI_STATUS_DUPLICATE_GSI_BASE) {
        return false;
    }

    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_local_apic(0U, 0U, ACPI_PROCESSOR_ENABLED);

    for (size_t index = 0; index <= ACPI_MAX_IO_APICS; ++index) {
        fixture_add_io_apic(
            (uint8_t)index,
            0U,
            UINT32_C(0xFEC00000) + (uint32_t)index * UINT32_C(0x1000),
            (uint32_t)index * UINT32_C(24)
        );
    }

    return fixture_status() == ACPI_STATUS_TOO_MANY_IO_APICS;
}

static bool interrupt_override_errors_are_rejected(void)
{
    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_minimum();
    fixture_add_interrupt_override(1U, 0U, 2U, 0U);

    if (fixture_status() != ACPI_STATUS_BAD_INTERRUPT_OVERRIDE_BUS) {
        return false;
    }

    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_minimum();
    fixture_add_interrupt_override(0U, 16U, 16U, 0U);

    if (fixture_status() != ACPI_STATUS_BAD_INTERRUPT_OVERRIDE_SOURCE) {
        return false;
    }

    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_minimum();
    fixture_add_interrupt_override(0U, 0U, 2U, UINT16_C(2));

    if (fixture_status() != ACPI_STATUS_BAD_INTERRUPT_FLAGS) {
        return false;
    }

    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_minimum();
    fixture_add_interrupt_override(0U, 0U, 2U, 0U);
    fixture_add_interrupt_override(0U, 0U, 3U, 0U);

    if (fixture_status() != ACPI_STATUS_DUPLICATE_INTERRUPT_SOURCE) {
        return false;
    }

    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_minimum();
    fixture_add_interrupt_override(0U, 0U, 2U, 0U);
    fixture_add_interrupt_override(0U, 1U, 2U, 0U);

    if (fixture_status() != ACPI_STATUS_DUPLICATE_INTERRUPT_GSI) {
        return false;
    }

    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_minimum();

    for (size_t index = 0;
         index <= ACPI_MAX_INTERRUPT_OVERRIDES;
         ++index) {
        fixture_add_interrupt_override(
            0U,
            (uint8_t)(index % TEST_ISA_INTERRUPT_COUNT),
            (uint32_t)index + UINT32_C(32),
            0U
        );
    }

    return fixture_status() == ACPI_STATUS_TOO_MANY_INTERRUPT_OVERRIDES;
}

static bool local_apic_override_errors_are_rejected(void)
{
    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_minimum();
    fixture_add_local_apic_override(1U, UINT64_C(0xFEE01000));

    if (fixture_status() !=
        ACPI_STATUS_BAD_LOCAL_APIC_OVERRIDE_RESERVED) {
        return false;
    }

    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_minimum();
    fixture_add_local_apic_override(0U, 0U);

    if (fixture_status() != ACPI_STATUS_NULL_LOCAL_APIC_ADDRESS) {
        return false;
    }

    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_minimum();
    fixture_add_local_apic_override(0U, UINT64_C(0xFEE01000));
    fixture_add_local_apic_override(0U, UINT64_C(0xFEE02000));

    return fixture_status() == ACPI_STATUS_DUPLICATE_LOCAL_APIC_OVERRIDE;
}

static bool required_topology_is_enforced(void)
{
    struct acpi_madt madt;

    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_io_apic(1U, 0U, UINT32_C(0xFEC00000), 0U);

    if (fixture_status() != ACPI_STATUS_MISSING_USABLE_PROCESSOR) {
        return false;
    }

    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_local_apic(0U, 0U, 0U);
    fixture_add_io_apic(1U, 0U, UINT32_C(0xFEC00000), 0U);

    if (fixture_status() != ACPI_STATUS_MISSING_USABLE_PROCESSOR) {
        return false;
    }

    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_local_apic(0U, 0U, ACPI_PROCESSOR_ENABLED);

    if (fixture_status() != ACPI_STATUS_MISSING_IO_APIC) {
        return false;
    }

    fixture_begin(0U);
    fixture_add_minimum();

    if (fixture_status() != ACPI_STATUS_NULL_LOCAL_APIC_ADDRESS) {
        return false;
    }

    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_minimum();
    madt = fixture_finish();
    test_fixture.bytes[9] ^= UINT8_C(1);

    return acpi_topology_discover(&madt, &test_topology) ==
        ACPI_STATUS_BAD_TABLE_CHECKSUM;
}

bool acpi_topology_self_test(void)
{
    struct acpi_madt madt;

    fixture_begin(UINT32_C(0xFEE00000));
    fixture_add_minimum();
    madt = fixture_finish();

    if (acpi_topology_discover(NULL, &test_topology) !=
            ACPI_STATUS_NULL_ARGUMENT ||
        acpi_topology_discover(&madt, NULL) != ACPI_STATUS_NULL_ARGUMENT) {
        return false;
    }

    return valid_fixture_is_accepted() &&
        entry_bounds_are_rejected() &&
        processor_errors_are_rejected() &&
        io_apic_errors_are_rejected() &&
        interrupt_override_errors_are_rejected() &&
        local_apic_override_errors_are_rejected() &&
        required_topology_is_enforced();
}
