/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <seneri/acpi.h>
#include <seneri/acpi_util.h>

/* ACPI 6.6 section 5.2.12 defines the MADT flag field. */
#define ACPI_MADT_PCAT_COMPAT UINT32_C(1)

/* Seneri early-boot policy bounds firmware-controlled work. */
#define ACPI_MAX_ROOT_ENTRIES 256U

struct acpi_sdt_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct acpi_madt_table {
    struct acpi_sdt_header header;
    uint32_t local_apic_address;
    uint32_t flags;
    uint8_t entries[];
} __attribute__((packed));

struct acpi_test_table {
    struct acpi_sdt_header header;
    uint32_t payload;
} __attribute__((packed));

struct acpi_test_madt {
    struct acpi_sdt_header header;
    uint32_t local_apic_address;
    uint32_t flags;
} __attribute__((packed));

struct acpi_test_xsdt {
    struct acpi_sdt_header header;
    uint64_t entries[2];
} __attribute__((packed));

struct acpi_test_rsdt {
    struct acpi_sdt_header header;
    uint32_t entries[2];
} __attribute__((packed));

struct acpi_test_fixture {
    struct acpi_test_table other;
    struct acpi_test_madt madt;
    struct acpi_test_xsdt xsdt;
    struct acpi_test_rsdt rsdt;
} __attribute__((aligned(8)));

_Static_assert(sizeof(struct acpi_sdt_header) == ACPI_SDT_HEADER_SIZE,
               "ACPI description header layout changed");
_Static_assert(offsetof(struct acpi_sdt_header, length) == 4U,
               "ACPI description length offset changed");
_Static_assert(offsetof(struct acpi_sdt_header, checksum) == 9U,
               "ACPI description checksum offset changed");
_Static_assert(sizeof(struct acpi_madt_table) == ACPI_MADT_FIXED_SIZE,
               "ACPI MADT fixed layout changed");
_Static_assert(offsetof(struct acpi_madt_table, local_apic_address) == 36U,
               "ACPI MADT local APIC address offset changed");
_Static_assert(offsetof(struct acpi_madt_table, flags) == 40U,
               "ACPI MADT flags offset changed");

static const char rsdt_signature[4] = {'R', 'S', 'D', 'T'};
static const char xsdt_signature[4] = {'X', 'S', 'D', 'T'};
static const char madt_signature[4] = {'A', 'P', 'I', 'C'};
static const char test_signature[4] = {'T', 'E', 'S', 'T'};
static const char test_oem_id[6] = {'Z', 'E', 'N', 'I', 'T', 'H'};
static const char test_oem_table_id[8] = {'Z', 'T', 'T', 'A', 'B', 'L', 'E', 'S'};

static void madt_reset(struct acpi_madt *madt)
{
    madt->physical_address = 0U;
    madt->length = 0U;
    madt->local_apic_address = 0U;
    madt->flags = 0U;
    madt->root_entry_count = 0U;
    madt->revision = 0U;

    for (size_t index = 0; index < sizeof(madt->oem_id); ++index) {
        madt->oem_id[index] = '\0';
    }

    for (size_t index = 0; index < sizeof(madt->oem_table_id); ++index) {
        madt->oem_table_id[index] = '\0';
    }
}

static enum acpi_status validate_root_table(
    const struct acpi_root *root,
    const struct acpi_sdt_header **header_out,
    size_t *entry_size_out,
    size_t *entry_count_out
)
{
    const struct acpi_sdt_header *header;
    const char *expected_signature;
    size_t entry_size;
    size_t payload_size;
    size_t entry_count;

    if (root->physical_address == 0U) {
        return ACPI_STATUS_NULL_ROOT;
    }

    if (!acpi_span_is_early_mapped(
            root->physical_address,
            ACPI_SDT_HEADER_SIZE
        )) {
        return ACPI_STATUS_ROOT_OUTSIDE_EARLY_MAP;
    }

    header = (const struct acpi_sdt_header *)(uintptr_t)root->physical_address;

    if (root->kind == ACPI_ROOT_XSDT) {
        expected_signature = xsdt_signature;
        entry_size = sizeof(uint64_t);
    } else if (root->kind == ACPI_ROOT_RSDT) {
        expected_signature = rsdt_signature;
        entry_size = sizeof(uint32_t);
    } else {
        return ACPI_STATUS_BAD_ROOT_SIGNATURE;
    }

    if (!acpi_bytes_equal(
            header->signature,
            expected_signature,
            sizeof(header->signature)
        )) {
        return ACPI_STATUS_BAD_ROOT_SIGNATURE;
    }

    if (header->length < sizeof(*header) ||
        header->length > ACPI_MAX_TABLE_SIZE) {
        return ACPI_STATUS_BAD_ROOT_LENGTH;
    }

    payload_size = header->length - sizeof(*header);

    if (payload_size % entry_size != 0U) {
        return ACPI_STATUS_BAD_ROOT_ENTRY_SIZE;
    }

    entry_count = payload_size / entry_size;

    if (entry_count > ACPI_MAX_ROOT_ENTRIES) {
        return ACPI_STATUS_TOO_MANY_ROOT_ENTRIES;
    }

    if (!acpi_span_is_early_mapped(root->physical_address, header->length)) {
        return ACPI_STATUS_ROOT_OUTSIDE_EARLY_MAP;
    }

    if (acpi_byte_sum(header, header->length) != 0U) {
        return ACPI_STATUS_BAD_ROOT_CHECKSUM;
    }

    *header_out = header;
    *entry_size_out = entry_size;
    *entry_count_out = entry_count;
    return ACPI_STATUS_OK;
}

static enum acpi_status validate_referenced_table(
    uint64_t physical_address,
    const struct acpi_sdt_header **header_out
)
{
    const struct acpi_sdt_header *header;

    if (physical_address == 0U) {
        return ACPI_STATUS_NULL_TABLE;
    }

    if (!acpi_span_is_early_mapped(physical_address, ACPI_SDT_HEADER_SIZE)) {
        return ACPI_STATUS_TABLE_OUTSIDE_EARLY_MAP;
    }

    header = (const struct acpi_sdt_header *)(uintptr_t)physical_address;

    if (header->length < sizeof(*header) ||
        header->length > ACPI_MAX_TABLE_SIZE) {
        return ACPI_STATUS_BAD_TABLE_LENGTH;
    }

    if (!acpi_span_is_early_mapped(physical_address, header->length)) {
        return ACPI_STATUS_TABLE_OUTSIDE_EARLY_MAP;
    }

    if (acpi_byte_sum(header, header->length) != 0U) {
        return ACPI_STATUS_BAD_TABLE_CHECKSUM;
    }

    *header_out = header;
    return ACPI_STATUS_OK;
}

enum acpi_status acpi_madt_discover(
    const struct acpi_root *root,
    struct acpi_madt *madt
)
{
    const struct acpi_sdt_header *root_header;
    const uint8_t *entries;
    size_t entry_size;
    size_t entry_count;
    bool found = false;
    enum acpi_status status;

    if (root == NULL || madt == NULL) {
        return ACPI_STATUS_NULL_ARGUMENT;
    }

    madt_reset(madt);
    status = validate_root_table(
        root,
        &root_header,
        &entry_size,
        &entry_count
    );

    if (status != ACPI_STATUS_OK) {
        return status;
    }

    entries = (const uint8_t *)(const void *)root_header + sizeof(*root_header);

    for (size_t index = 0; index < entry_count; ++index) {
        const uint8_t *entry = entries + index * entry_size;
        const struct acpi_sdt_header *header;
        uint64_t physical_address = entry_size == sizeof(uint64_t)
            ? acpi_read_u64(entry)
            : acpi_read_u32(entry);

        status = validate_referenced_table(physical_address, &header);

        if (status != ACPI_STATUS_OK) {
            madt_reset(madt);
            return status;
        }

        if (!acpi_bytes_equal(
                header->signature,
                madt_signature,
                sizeof(header->signature)
            )) {
            continue;
        }

        if (found) {
            madt_reset(madt);
            return ACPI_STATUS_DUPLICATE_MADT;
        }

        if (header->length < sizeof(struct acpi_madt_table)) {
            madt_reset(madt);
            return ACPI_STATUS_BAD_MADT_LENGTH;
        }

        {
            const struct acpi_madt_table *table =
                (const struct acpi_madt_table *)(const void *)header;

            if ((table->flags & ~ACPI_MADT_PCAT_COMPAT) != 0U) {
                madt_reset(madt);
                return ACPI_STATUS_BAD_MADT_FLAGS;
            }

            madt->physical_address = physical_address;
            madt->length = header->length;
            madt->local_apic_address = table->local_apic_address;
            madt->flags = table->flags;
            madt->root_entry_count = entry_count;
            madt->revision = header->revision;
            acpi_copy_string(
                madt->oem_id,
                header->oem_id,
                sizeof(header->oem_id)
            );
            acpi_copy_string(
                madt->oem_table_id,
                header->oem_table_id,
                sizeof(header->oem_table_id)
            );
            found = true;
        }
    }

    if (!found) {
        madt_reset(madt);
        return ACPI_STATUS_MISSING_MADT;
    }

    return ACPI_STATUS_OK;
}

static void initialize_header(
    struct acpi_sdt_header *header,
    const char signature[4],
    uint32_t length
)
{
    acpi_bytes_zero(header, sizeof(*header));

    for (size_t index = 0; index < sizeof(header->signature); ++index) {
        header->signature[index] = signature[index];
    }

    for (size_t index = 0; index < sizeof(header->oem_id); ++index) {
        header->oem_id[index] = test_oem_id[index];
    }

    for (size_t index = 0; index < sizeof(header->oem_table_id); ++index) {
        header->oem_table_id[index] = test_oem_table_id[index];
    }

    header->length = length;
    header->revision = 1U;
    header->oem_revision = 1U;
    header->creator_id = UINT32_C(0x54494E5A);
    header->creator_revision = 1U;
}

static void set_checksum(void *table, size_t size)
{
    struct acpi_sdt_header *header = (struct acpi_sdt_header *)table;

    header->checksum = 0U;
    header->checksum = (uint8_t)(0U - acpi_byte_sum(table, size));
}

static void prepare_test_fixture(struct acpi_test_fixture *fixture)
{
    acpi_bytes_zero(fixture, sizeof(*fixture));

    initialize_header(
        &fixture->other.header,
        test_signature,
        sizeof(fixture->other)
    );
    fixture->other.payload = UINT32_C(0x12345678);
    set_checksum(&fixture->other, sizeof(fixture->other));

    initialize_header(
        &fixture->madt.header,
        madt_signature,
        sizeof(fixture->madt)
    );
    fixture->madt.header.revision = 7U;
    fixture->madt.local_apic_address = UINT32_C(0xFEE00000);
    fixture->madt.flags = ACPI_MADT_PCAT_COMPAT;
    set_checksum(&fixture->madt, sizeof(fixture->madt));

    initialize_header(
        &fixture->xsdt.header,
        xsdt_signature,
        sizeof(fixture->xsdt)
    );
    fixture->xsdt.entries[0] =
        (uint64_t)(uintptr_t)(void *)&fixture->other;
    fixture->xsdt.entries[1] =
        (uint64_t)(uintptr_t)(void *)&fixture->madt;
    set_checksum(&fixture->xsdt, sizeof(fixture->xsdt));

    initialize_header(
        &fixture->rsdt.header,
        rsdt_signature,
        sizeof(fixture->rsdt)
    );
    fixture->rsdt.entries[0] =
        (uint32_t)(uintptr_t)(void *)&fixture->other;
    fixture->rsdt.entries[1] =
        (uint32_t)(uintptr_t)(void *)&fixture->madt;
    set_checksum(&fixture->rsdt, sizeof(fixture->rsdt));
}

static struct acpi_root test_root(
    const struct acpi_test_fixture *fixture,
    enum acpi_root_kind kind
)
{
    struct acpi_root root = {
        .kind = kind,
        .revision = 2U,
        .oem_id = {'Z', 'E', 'N', 'I', 'T', 'H', '\0'},
        .physical_address = kind == ACPI_ROOT_XSDT
            ? (uint64_t)(uintptr_t)(const void *)&fixture->xsdt
            : (uint64_t)(uintptr_t)(const void *)&fixture->rsdt
    };

    return root;
}

static bool valid_fixture_is_accepted(void)
{
    struct acpi_test_fixture fixture;
    struct acpi_madt madt;
    struct acpi_root root;

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);

    if (acpi_madt_discover(&root, &madt) != ACPI_STATUS_OK ||
        madt.physical_address !=
            (uint64_t)(uintptr_t)(void *)&fixture.madt ||
        madt.length != sizeof(fixture.madt) ||
        madt.local_apic_address != UINT32_C(0xFEE00000) ||
        madt.flags != ACPI_MADT_PCAT_COMPAT ||
        madt.root_entry_count != 2U ||
        madt.revision != 7U ||
        !acpi_bytes_equal(madt.oem_id, test_oem_id, sizeof(test_oem_id)) ||
        !acpi_bytes_equal(
            madt.oem_table_id,
            test_oem_table_id,
            sizeof(test_oem_table_id)
        )) {
        return false;
    }

    root = test_root(&fixture, ACPI_ROOT_RSDT);
    return acpi_madt_discover(&root, &madt) == ACPI_STATUS_OK &&
        madt.physical_address == (uint64_t)(uintptr_t)(void *)&fixture.madt;
}

bool acpi_tables_self_test(void)
{
    struct acpi_test_fixture fixture;
    struct acpi_madt madt;
    struct acpi_root root;

    if (!valid_fixture_is_accepted()) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.xsdt.header.signature[0] = 'B';

    if (acpi_madt_discover(&root, &madt) != ACPI_STATUS_BAD_ROOT_SIGNATURE) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    root.physical_address = 0U;

    if (acpi_madt_discover(&root, &madt) != ACPI_STATUS_NULL_ROOT) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    root.physical_address =
        SENERI_EARLY_PHYSICAL_LIMIT - ACPI_SDT_HEADER_SIZE + 1U;

    if (acpi_madt_discover(&root, &madt) !=
        ACPI_STATUS_ROOT_OUTSIDE_EARLY_MAP) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.xsdt.header.length = sizeof(struct acpi_sdt_header) - 1U;

    if (acpi_madt_discover(&root, &madt) != ACPI_STATUS_BAD_ROOT_LENGTH) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.xsdt.header.length = sizeof(struct acpi_sdt_header) + 1U;

    if (acpi_madt_discover(&root, &madt) != ACPI_STATUS_BAD_ROOT_ENTRY_SIZE) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.xsdt.header.length = sizeof(struct acpi_sdt_header) +
        (ACPI_MAX_ROOT_ENTRIES + 1U) * sizeof(uint64_t);

    if (acpi_madt_discover(&root, &madt) !=
        ACPI_STATUS_TOO_MANY_ROOT_ENTRIES) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.xsdt.header.checksum ^= UINT8_C(1);

    if (acpi_madt_discover(&root, &madt) != ACPI_STATUS_BAD_ROOT_CHECKSUM) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.xsdt.entries[0] = 0U;
    set_checksum(&fixture.xsdt, sizeof(fixture.xsdt));

    if (acpi_madt_discover(&root, &madt) != ACPI_STATUS_NULL_TABLE) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.xsdt.entries[0] =
        SENERI_EARLY_PHYSICAL_LIMIT - ACPI_SDT_HEADER_SIZE + 1U;
    set_checksum(&fixture.xsdt, sizeof(fixture.xsdt));

    if (acpi_madt_discover(&root, &madt) !=
        ACPI_STATUS_TABLE_OUTSIDE_EARLY_MAP) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.other.header.length = sizeof(struct acpi_sdt_header) - 1U;

    if (acpi_madt_discover(&root, &madt) != ACPI_STATUS_BAD_TABLE_LENGTH) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.other.header.checksum ^= UINT8_C(1);

    if (acpi_madt_discover(&root, &madt) != ACPI_STATUS_BAD_TABLE_CHECKSUM) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.xsdt.header.length = sizeof(struct acpi_sdt_header) + sizeof(uint64_t);
    set_checksum(&fixture.xsdt, fixture.xsdt.header.length);

    if (acpi_madt_discover(&root, &madt) != ACPI_STATUS_MISSING_MADT) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.xsdt.entries[0] =
        (uint64_t)(uintptr_t)(void *)&fixture.madt;
    set_checksum(&fixture.xsdt, sizeof(fixture.xsdt));

    if (acpi_madt_discover(&root, &madt) != ACPI_STATUS_DUPLICATE_MADT) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.madt.header.length = sizeof(struct acpi_sdt_header);
    set_checksum(&fixture.madt, fixture.madt.header.length);

    if (acpi_madt_discover(&root, &madt) != ACPI_STATUS_BAD_MADT_LENGTH) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.madt.flags = UINT32_C(2);
    set_checksum(&fixture.madt, sizeof(fixture.madt));

    return acpi_madt_discover(&root, &madt) == ACPI_STATUS_BAD_MADT_FLAGS;
}
