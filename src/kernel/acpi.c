/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zenith/acpi.h>

#define ACPI_RSDP_V1_SIZE 20U
#define ACPI_RSDP_V2_SIZE 36U
#define ACPI_ROOT_HEADER_SIZE 36U

struct acpi_rsdp {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed));

struct acpi_test_tag {
    struct multiboot2_tag tag;
    struct acpi_rsdp rsdp;
    uint32_t padding;
} __attribute__((packed, aligned(8)));

_Static_assert(sizeof(struct acpi_rsdp) == ACPI_RSDP_V2_SIZE,
               "ACPI RSDP layout changed");
_Static_assert(offsetof(struct acpi_rsdp, length) == ACPI_RSDP_V1_SIZE,
               "ACPI 1.0 RSDP prefix changed");

static const char rsdp_signature[8] = {'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '};
static const char test_oem_id[6] = {'Z', 'E', 'N', 'I', 'T', 'H'};

static bool bytes_equal(const void *left, const void *right, size_t size)
{
    const uint8_t *left_bytes = (const uint8_t *)left;
    const uint8_t *right_bytes = (const uint8_t *)right;

    for (size_t index = 0; index < size; ++index) {
        if (left_bytes[index] != right_bytes[index]) {
            return false;
        }
    }

    return true;
}

static uint8_t byte_sum(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint8_t sum = 0U;

    for (size_t index = 0; index < size; ++index) {
        sum = (uint8_t)(sum + bytes[index]);
    }

    return sum;
}

static void root_reset(struct acpi_root *root)
{
    root->kind = ACPI_ROOT_NONE;
    root->revision = 0U;
    root->physical_address = 0U;

    for (size_t index = 0; index < sizeof(root->oem_id); ++index) {
        root->oem_id[index] = '\0';
    }
}

enum acpi_status acpi_root_discover(
    const struct boot_context *context,
    struct acpi_root *root
)
{
    const struct multiboot2_acpi_tag *tag;
    const struct acpi_rsdp *rsdp;
    size_t payload_size;
    uint64_t address;
    bool modern;

    if (context == NULL || root == NULL) {
        return ACPI_STATUS_NULL_ARGUMENT;
    }

    root_reset(root);
    modern = context->acpi_new != NULL;
    tag = modern ? context->acpi_new : context->acpi_old;

    if (tag == NULL) {
        return ACPI_STATUS_MISSING_RSDP;
    }

    if (tag->tag.type != (modern ? MULTIBOOT2_TAG_ACPI_NEW
                                 : MULTIBOOT2_TAG_ACPI_OLD) ||
        tag->tag.size < sizeof(tag->tag) +
            (modern ? ACPI_RSDP_V2_SIZE : ACPI_RSDP_V1_SIZE)) {
        return ACPI_STATUS_BAD_TAG_SIZE;
    }

    payload_size = tag->tag.size - sizeof(tag->tag);
    rsdp = (const struct acpi_rsdp *)(const void *)tag->rsdp;

    if (!bytes_equal(rsdp->signature, rsdp_signature, sizeof(rsdp_signature))) {
        return ACPI_STATUS_BAD_SIGNATURE;
    }

    if (byte_sum(rsdp, ACPI_RSDP_V1_SIZE) != 0U) {
        return ACPI_STATUS_BAD_LEGACY_CHECKSUM;
    }

    if (modern) {
        if (rsdp->revision < 2U) {
            return ACPI_STATUS_BAD_REVISION;
        }

        if (rsdp->length < ACPI_RSDP_V2_SIZE || rsdp->length > payload_size) {
            return ACPI_STATUS_BAD_LENGTH;
        }

        if (byte_sum(rsdp, rsdp->length) != 0U) {
            return ACPI_STATUS_BAD_EXTENDED_CHECKSUM;
        }

        root->kind = rsdp->xsdt_address != 0U
            ? ACPI_ROOT_XSDT
            : ACPI_ROOT_RSDT;
        address = rsdp->xsdt_address != 0U
            ? rsdp->xsdt_address
            : rsdp->rsdt_address;
    } else {
        if (rsdp->revision != 0U) {
            return ACPI_STATUS_BAD_REVISION;
        }

        root->kind = ACPI_ROOT_RSDT;
        address = rsdp->rsdt_address;
    }

    if (address == 0U) {
        root_reset(root);
        return ACPI_STATUS_NULL_ROOT;
    }

    if (address > ZENITH_EARLY_PHYSICAL_LIMIT - ACPI_ROOT_HEADER_SIZE) {
        root_reset(root);
        return ACPI_STATUS_ROOT_OUTSIDE_EARLY_MAP;
    }

    root->revision = rsdp->revision;
    root->physical_address = address;

    for (size_t index = 0; index < sizeof(rsdp->oem_id); ++index) {
        root->oem_id[index] = rsdp->oem_id[index];
    }

    return ACPI_STATUS_OK;
}

static void make_test_tag(struct acpi_test_tag *tag, bool modern)
{
    uint8_t *bytes = (uint8_t *)(void *)tag;

    for (size_t index = 0; index < sizeof(*tag); ++index) {
        bytes[index] = 0U;
    }

    tag->tag.type = modern ? MULTIBOOT2_TAG_ACPI_NEW : MULTIBOOT2_TAG_ACPI_OLD;
    tag->tag.size = sizeof(tag->tag) +
        (modern ? ACPI_RSDP_V2_SIZE : ACPI_RSDP_V1_SIZE);

    for (size_t index = 0; index < sizeof(rsdp_signature); ++index) {
        tag->rsdp.signature[index] = rsdp_signature[index];
    }

    for (size_t index = 0; index < sizeof(test_oem_id); ++index) {
        tag->rsdp.oem_id[index] = test_oem_id[index];
    }

    tag->rsdp.revision = modern ? 2U : 0U;
    tag->rsdp.rsdt_address = UINT32_C(0x12345000);
    tag->rsdp.length = ACPI_RSDP_V2_SIZE;
    tag->rsdp.xsdt_address = UINT64_C(0x23456000);
    tag->rsdp.checksum = (uint8_t)(0U - byte_sum(&tag->rsdp, ACPI_RSDP_V1_SIZE));
    tag->rsdp.extended_checksum = (uint8_t)(
        0U - byte_sum(&tag->rsdp, ACPI_RSDP_V2_SIZE)
    );
}

bool acpi_self_test(void)
{
    struct acpi_test_tag tag;
    struct boot_context context = {0};
    struct acpi_root root;

    make_test_tag(&tag, true);
    context.acpi_new =
        (const struct multiboot2_acpi_tag *)(const void *)&tag;

    if (acpi_root_discover(&context, &root) != ACPI_STATUS_OK ||
        root.kind != ACPI_ROOT_XSDT ||
        root.physical_address != UINT64_C(0x23456000) ||
        !bytes_equal(root.oem_id, test_oem_id, sizeof(test_oem_id))) {
        return false;
    }

    tag.rsdp.signature[0] = 'B';

    if (acpi_root_discover(&context, &root) != ACPI_STATUS_BAD_SIGNATURE) {
        return false;
    }

    make_test_tag(&tag, true);
    tag.rsdp.checksum ^= UINT8_C(1);

    if (acpi_root_discover(&context, &root) !=
        ACPI_STATUS_BAD_LEGACY_CHECKSUM) {
        return false;
    }

    make_test_tag(&tag, true);
    tag.rsdp.length = ACPI_RSDP_V2_SIZE - 1U;

    if (acpi_root_discover(&context, &root) != ACPI_STATUS_BAD_LENGTH) {
        return false;
    }

    make_test_tag(&tag, true);
    tag.rsdp.extended_checksum ^= UINT8_C(1);

    if (acpi_root_discover(&context, &root) !=
        ACPI_STATUS_BAD_EXTENDED_CHECKSUM) {
        return false;
    }

    make_test_tag(&tag, false);
    context.acpi_new = NULL;
    context.acpi_old =
        (const struct multiboot2_acpi_tag *)(const void *)&tag;

    return acpi_root_discover(&context, &root) == ACPI_STATUS_OK &&
        root.kind == ACPI_ROOT_RSDT &&
        root.physical_address == UINT32_C(0x12345000);
}

const char *acpi_root_kind_string(enum acpi_root_kind kind)
{
    return kind == ACPI_ROOT_XSDT ? "XSDT" :
        (kind == ACPI_ROOT_RSDT ? "RSDT" : "none");
}

const char *acpi_status_string(enum acpi_status status)
{
    static const char *const messages[] = {
        "ok",
        "null ACPI discovery argument",
        "missing ACPI RSDP",
        "ACPI RSDP tag is too small",
        "invalid ACPI RSDP signature",
        "invalid ACPI 1.0 checksum",
        "ACPI RSDP revision disagrees with its tag",
        "invalid ACPI RSDP length",
        "invalid ACPI extended checksum",
        "ACPI root table address is null",
        "ACPI root table is outside the early map",
        "ACPI root table signature does not match the RSDP",
        "invalid ACPI root table length",
        "ACPI root table has a partial entry",
        "ACPI root table exceeds the early entry limit",
        "invalid ACPI root table checksum",
        "ACPI root table contains a null table address",
        "ACPI table is outside the early map",
        "invalid ACPI table length",
        "invalid ACPI table checksum",
        "ACPI root table does not contain a MADT",
        "ACPI root table contains duplicate MADTs",
        "ACPI MADT is shorter than its fixed header",
        "ACPI MADT contains nonzero reserved flags",
        "ACPI MADT ends with a partial entry header",
        "ACPI MADT entry has an invalid length",
        "ACPI MADT exceeds the early entry limit",
        "ACPI processor entry has invalid flags",
        "ACPI processor entry has nonzero reserved fields",
        "ACPI MADT exceeds the processor limit",
        "ACPI MADT contains duplicate processor UIDs",
        "ACPI MADT contains duplicate APIC IDs",
        "ACPI I/O APIC entry has nonzero reserved fields",
        "ACPI I/O APIC address is null",
        "ACPI MADT exceeds the I/O APIC limit",
        "ACPI MADT contains duplicate I/O APIC IDs",
        "ACPI MADT contains duplicate I/O APIC addresses",
        "ACPI MADT contains duplicate global interrupt bases",
        "ACPI interrupt override does not describe the ISA bus",
        "ACPI interrupt override source is outside ISA IRQ0-15",
        "ACPI interrupt entry has invalid polarity or trigger flags",
        "ACPI MADT exceeds the interrupt override limit",
        "ACPI MADT contains duplicate interrupt override sources",
        "ACPI MADT contains duplicate interrupt override GSIs",
        "ACPI local APIC override has nonzero reserved fields",
        "ACPI local APIC address is null",
        "ACPI MADT contains duplicate local APIC address overrides",
        "ACPI MADT contains no usable processor",
        "ACPI MADT contains no I/O APIC"
    };

    _Static_assert(
        sizeof(messages) / sizeof(messages[0]) ==
            (size_t)ACPI_STATUS_MISSING_IO_APIC + 1U,
        "ACPI status messages are out of sync"
    );

    if ((size_t)status >= sizeof(messages) / sizeof(messages[0])) {
        return "unknown ACPI status";
    }

    return messages[status];
}
