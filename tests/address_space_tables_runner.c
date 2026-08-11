/* SPDX-License-Identifier: GPL-3.0-only */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/kernel/address_space_tables.h"

#define ENTRY_PRESENT (UINT64_C(1) << 0U)
#define ENTRY_WRITABLE (UINT64_C(1) << 1U)
#define ENTRY_USER (UINT64_C(1) << 2U)
#define ENTRY_ACCESSED (UINT64_C(1) << 5U)
#define ENTRY_DIRTY (UINT64_C(1) << 6U)
#define ENTRY_NO_EXECUTE (UINT64_C(1) << 63U)

#define PHYSICAL_MASK UINT64_C(0x0000000FFFFFF000)
#define KERNEL_PML4_PHYSICAL UINT64_C(0x01000000)
#define KERNEL_PDPT_PHYSICAL UINT64_C(0x01001000)
#define KERNEL_PD_PHYSICAL UINT64_C(0x01002000)
#define KERNEL_HIGH_0_PHYSICAL UINT64_C(0x01003000)
#define KERNEL_HIGH_1_PHYSICAL UINT64_C(0x01004000)
#define KERNEL_HIGH_2_PHYSICAL UINT64_C(0x01005000)
#define USER_FRAME_BASE UINT64_C(0x03000000)

static const uint64_t private_physical[ADDRESS_SPACE_TABLE_PAGE_COUNT] = {
    UINT64_C(0x02000000),
    UINT64_C(0x02001000),
    UINT64_C(0x02002000),
    UINT64_C(0x02003000)
};

static struct address_space_table_storage storage;
static struct address_space_tables tables;
static _Alignas(ADDRESS_SPACE_TABLE_PAGE_SIZE)
    uint64_t kernel_pml4[ADDRESS_SPACE_TABLE_ENTRY_COUNT];
static _Alignas(ADDRESS_SPACE_TABLE_PAGE_SIZE)
    uint64_t kernel_low_pdpt[ADDRESS_SPACE_TABLE_ENTRY_COUNT];
static struct address_space_table_kernel_template kernel_template;

static void template_reset(void)
{
    memset(kernel_pml4, 0, sizeof(kernel_pml4));
    memset(kernel_low_pdpt, 0, sizeof(kernel_low_pdpt));
    kernel_pml4[0] = KERNEL_PDPT_PHYSICAL |
        ENTRY_PRESENT | ENTRY_WRITABLE;
    kernel_pml4[256] = KERNEL_HIGH_0_PHYSICAL |
        ENTRY_PRESENT | ENTRY_WRITABLE;
    kernel_pml4[288] = KERNEL_HIGH_1_PHYSICAL |
        ENTRY_PRESENT | ENTRY_WRITABLE;
    kernel_pml4[320] = KERNEL_HIGH_2_PHYSICAL |
        ENTRY_PRESENT | ENTRY_WRITABLE;
    kernel_low_pdpt[0] = KERNEL_PD_PHYSICAL |
        ENTRY_PRESENT | ENTRY_WRITABLE;
    kernel_template.pml4 = kernel_pml4;
    kernel_template.low_pdpt = kernel_low_pdpt;
    kernel_template.pml4_physical_address = KERNEL_PML4_PHYSICAL;
    kernel_template.low_pdpt_physical_address = KERNEL_PDPT_PHYSICAL;
    kernel_template.physical_address_mask = PHYSICAL_MASK;
}
static bool storage_is_zero(const struct address_space_table_storage *value)
{
    const uint8_t *bytes = (const uint8_t *)(const void *)value;

    for (size_t index = 0U; index < sizeof(*value); ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool state_is_zero(const struct address_space_tables *value)
{
    const uint8_t *bytes = (const uint8_t *)(const void *)value;

    for (size_t index = 0U; index < sizeof(*value); ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static void reset_all(void)
{
    template_reset();
    address_space_tables_clear(&tables, &storage);
}

static enum address_space_table_status build_current(void)
{
    return address_space_tables_build(
        &tables, &storage, private_physical, &kernel_template
    );
}

static bool outputs_clear_on_query_failure(void)
{
    struct address_space_table_mapping mapping = {
        .physical_address = UINT64_MAX,
        .permissions = UINT32_MAX,
        .accessed = true,
        .dirty = true,
        .reserved = {UINT8_MAX, UINT8_MAX}
    };

    return address_space_tables_query(
            &tables,
            ADDRESS_SPACE_TABLE_USER_BASE,
            &mapping
        ) == ADDRESS_SPACE_TABLE_STATUS_MAPPING_NOT_FOUND &&
        mapping.physical_address == 0U && mapping.permissions == 0U &&
        !mapping.accessed && !mapping.dirty &&
        mapping.reserved[0] == 0U && mapping.reserved[1] == 0U;
}

static bool failed_build_is_clean(void)
{
    return state_is_zero(&tables) && storage_is_zero(&storage);
}

static bool malformed_build_rejections(void)
{
    uint64_t bad_physical[ADDRESS_SPACE_TABLE_PAGE_COUNT];
    struct address_space_tables *aliased_state;

    for (size_t index = 0U;
         index < ADDRESS_SPACE_TABLE_PAGE_COUNT;
         ++index) {
        bad_physical[index] = private_physical[index];
    }

    reset_all();
    kernel_pml4[0] |= ENTRY_USER;
    if (build_current() != ADDRESS_SPACE_TABLE_STATUS_INVALID_TEMPLATE ||
        !failed_build_is_clean()) {
        return false;
    }

    reset_all();
    kernel_pml4[1] = KERNEL_HIGH_0_PHYSICAL | ENTRY_PRESENT;
    if (build_current() != ADDRESS_SPACE_TABLE_STATUS_INVALID_TEMPLATE ||
        !failed_build_is_clean()) {
        return false;
    }

    reset_all();
    kernel_pml4[256] |= ENTRY_USER;
    if (build_current() != ADDRESS_SPACE_TABLE_STATUS_INVALID_TEMPLATE ||
        !failed_build_is_clean()) {
        return false;
    }

    reset_all();
    kernel_low_pdpt[1] = KERNEL_HIGH_0_PHYSICAL | ENTRY_PRESENT;
    if (build_current() != ADDRESS_SPACE_TABLE_STATUS_INVALID_TEMPLATE ||
        !failed_build_is_clean()) {
        return false;
    }

    reset_all();
    kernel_template.physical_address_mask = UINT64_C(0x00F0F000);
    if (build_current() != ADDRESS_SPACE_TABLE_STATUS_INVALID_TEMPLATE ||
        !failed_build_is_clean()) {
        return false;
    }

    reset_all();
    bad_physical[1] = bad_physical[0];
    if (address_space_tables_build(
            &tables, &storage, bad_physical, &kernel_template
        ) != ADDRESS_SPACE_TABLE_STATUS_INVALID_TEMPLATE ||
        !failed_build_is_clean()) {
        return false;
    }

    reset_all();
    bad_physical[1] = private_physical[1];
    bad_physical[2] = KERNEL_PD_PHYSICAL;
    if (address_space_tables_build(
            &tables, &storage, bad_physical, &kernel_template
        ) != ADDRESS_SPACE_TABLE_STATUS_INVALID_TEMPLATE ||
        !failed_build_is_clean()) {
        return false;
    }

    reset_all();
    aliased_state = (struct address_space_tables *)(void *)&kernel_pml4[8];
    if (address_space_tables_build(
            aliased_state, &storage, private_physical, &kernel_template
        ) != ADDRESS_SPACE_TABLE_STATUS_INVALID_TEMPLATE ||
        !storage_is_zero(&storage) ||
        !state_is_zero(aliased_state)) {
        return false;
    }

    reset_all();
    storage.entries[0][17] = UINT64_C(1);
    if (build_current() != ADDRESS_SPACE_TABLE_STATUS_NOT_CLEAR ||
        storage.entries[0][17] != UINT64_C(1) || !state_is_zero(&tables)) {
        return false;
    }

    reset_all();
    tables.mapping_count = 1U;
    if (build_current() != ADDRESS_SPACE_TABLE_STATUS_NOT_CLEAR ||
        tables.mapping_count != 1U || !storage_is_zero(&storage)) {
        return false;
    }

    reset_all();
    return address_space_tables_build(
            NULL, &storage, private_physical, &kernel_template
        ) == ADDRESS_SPACE_TABLE_STATUS_NULL_ARGUMENT &&
        address_space_tables_build(
            &tables, NULL, private_physical, &kernel_template
        ) == ADDRESS_SPACE_TABLE_STATUS_NULL_ARGUMENT &&
        address_space_tables_build(
            &tables, &storage, NULL, &kernel_template
        ) == ADDRESS_SPACE_TABLE_STATUS_NULL_ARGUMENT &&
        address_space_tables_build(
            &tables, &storage, private_physical, NULL
        ) == ADDRESS_SPACE_TABLE_STATUS_NULL_ARGUMENT &&
        failed_build_is_clean();
}

static bool mapping_rejections_and_lifecycle(void)
{
    const uint32_t rx = ADDRESS_SPACE_TABLE_PERMISSION_READ |
        ADDRESS_SPACE_TABLE_PERMISSION_EXECUTE |
        ADDRESS_SPACE_TABLE_PERMISSION_USER;
    const uint32_t rw = ADDRESS_SPACE_TABLE_PERMISSION_READ |
        ADDRESS_SPACE_TABLE_PERMISSION_WRITE |
        ADDRESS_SPACE_TABLE_PERMISSION_USER;
    struct address_space_table_mapping mapping;
    uint64_t *leaf;

    reset_all();
    if (build_current() != ADDRESS_SPACE_TABLE_STATUS_OK ||
        address_space_tables_validate(&tables) !=
            ADDRESS_SPACE_TABLE_STATUS_OK ||
        !outputs_clear_on_query_failure()) {
        return false;
    }

    if (address_space_tables_map(
            &tables, ADDRESS_SPACE_TABLE_USER_BASE,
            USER_FRAME_BASE, rx
        ) != ADDRESS_SPACE_TABLE_STATUS_OK ||
        address_space_tables_query(
            &tables, ADDRESS_SPACE_TABLE_USER_BASE, &mapping
        ) != ADDRESS_SPACE_TABLE_STATUS_OK ||
        mapping.physical_address != USER_FRAME_BASE ||
        mapping.permissions != rx || mapping.accessed || mapping.dirty ||
        address_space_tables_map(
            &tables, ADDRESS_SPACE_TABLE_USER_BASE,
            USER_FRAME_BASE + ADDRESS_SPACE_TABLE_PAGE_SIZE, rw
        ) != ADDRESS_SPACE_TABLE_STATUS_MAPPING_EXISTS ||
        address_space_tables_map(
            &tables,
            ADDRESS_SPACE_TABLE_USER_BASE + ADDRESS_SPACE_TABLE_PAGE_SIZE,
            USER_FRAME_BASE,
            rw
        ) != ADDRESS_SPACE_TABLE_STATUS_FRAME_IN_USE) {
        return false;
    }

    if (address_space_tables_map(
            &tables,
            ADDRESS_SPACE_TABLE_STACK_GUARD,
            USER_FRAME_BASE + ADDRESS_SPACE_TABLE_PAGE_SIZE,
            rw
        ) != ADDRESS_SPACE_TABLE_STATUS_GUARD_PAGE ||
        address_space_tables_map(
            &tables, 0U,
            USER_FRAME_BASE + ADDRESS_SPACE_TABLE_PAGE_SIZE,
            rw
        ) != ADDRESS_SPACE_TABLE_STATUS_NULL_PAGE ||
        address_space_tables_map(
            &tables, UINT64_C(0x0000800000000000),
            USER_FRAME_BASE + ADDRESS_SPACE_TABLE_PAGE_SIZE,
            rw
        ) != ADDRESS_SPACE_TABLE_STATUS_NONCANONICAL_ADDRESS ||
        address_space_tables_map(
            &tables, ADDRESS_SPACE_TABLE_USER_END,
            USER_FRAME_BASE + ADDRESS_SPACE_TABLE_PAGE_SIZE,
            rw
        ) != ADDRESS_SPACE_TABLE_STATUS_OUTSIDE_USER_APERTURE ||
        address_space_tables_map(
            &tables, ADDRESS_SPACE_TABLE_USER_BASE + 1U,
            USER_FRAME_BASE + ADDRESS_SPACE_TABLE_PAGE_SIZE,
            rw
        ) != ADDRESS_SPACE_TABLE_STATUS_BAD_ALIGNMENT ||
        address_space_tables_map(
            &tables,
            ADDRESS_SPACE_TABLE_USER_BASE + ADDRESS_SPACE_TABLE_PAGE_SIZE,
            USER_FRAME_BASE + ADDRESS_SPACE_TABLE_PAGE_SIZE,
            rw | ADDRESS_SPACE_TABLE_PERMISSION_EXECUTE
        ) != ADDRESS_SPACE_TABLE_STATUS_INVALID_PERMISSIONS ||
        address_space_tables_map(
            &tables,
            ADDRESS_SPACE_TABLE_USER_BASE + ADDRESS_SPACE_TABLE_PAGE_SIZE,
            private_physical[0],
            rw
        ) != ADDRESS_SPACE_TABLE_STATUS_FRAME_ALIAS ||
        address_space_tables_map(
            &tables,
            ADDRESS_SPACE_TABLE_USER_BASE + ADDRESS_SPACE_TABLE_PAGE_SIZE,
            USER_FRAME_BASE + 1U,
            rw
        ) != ADDRESS_SPACE_TABLE_STATUS_BAD_PHYSICAL_ADDRESS) {
        return false;
    }

    leaf = &storage.entries[ADDRESS_SPACE_TABLE_USER_PT][0];
    *leaf |= ENTRY_ACCESSED | ENTRY_DIRTY;
    if (address_space_tables_repermission(
            &tables, ADDRESS_SPACE_TABLE_USER_BASE, rw
        ) != ADDRESS_SPACE_TABLE_STATUS_OK ||
        address_space_tables_query(
            &tables, ADDRESS_SPACE_TABLE_USER_BASE, &mapping
        ) != ADDRESS_SPACE_TABLE_STATUS_OK ||
        mapping.permissions != rw || !mapping.accessed || !mapping.dirty ||
        address_space_tables_unmap(
            &tables, ADDRESS_SPACE_TABLE_USER_BASE,
            USER_FRAME_BASE + ADDRESS_SPACE_TABLE_PAGE_SIZE
        ) != ADDRESS_SPACE_TABLE_STATUS_MAPPING_MISMATCH ||
        address_space_tables_unmap(
            &tables, ADDRESS_SPACE_TABLE_USER_BASE, USER_FRAME_BASE
        ) != ADDRESS_SPACE_TABLE_STATUS_OK ||
        address_space_tables_unmap(
            &tables, ADDRESS_SPACE_TABLE_USER_BASE, USER_FRAME_BASE
        ) != ADDRESS_SPACE_TABLE_STATUS_MAPPING_NOT_FOUND ||
        tables.mapping_count != 0U ||
        address_space_tables_validate(&tables) !=
            ADDRESS_SPACE_TABLE_STATUS_OK) {
        return false;
    }
    return true;
}

static bool mapping_limit_and_corruption(void)
{
    const uint32_t rw = ADDRESS_SPACE_TABLE_PERMISSION_READ |
        ADDRESS_SPACE_TABLE_PERMISSION_WRITE |
        ADDRESS_SPACE_TABLE_PERMISSION_USER;
    struct address_space_table_mapping mapping;

    reset_all();
    if (build_current() != ADDRESS_SPACE_TABLE_STATUS_OK) {
        return false;
    }
    for (uint32_t index = 0U;
         index < ADDRESS_SPACE_TABLE_MAPPING_LIMIT;
         ++index) {
        if (address_space_tables_map(
                &tables,
                ADDRESS_SPACE_TABLE_USER_BASE +
                    (uint64_t)index * ADDRESS_SPACE_TABLE_PAGE_SIZE,
                USER_FRAME_BASE +
                    (uint64_t)index * ADDRESS_SPACE_TABLE_PAGE_SIZE,
                rw
            ) != ADDRESS_SPACE_TABLE_STATUS_OK) {
            return false;
        }
    }
    if (address_space_tables_map(
            &tables,
            ADDRESS_SPACE_TABLE_USER_BASE +
                (uint64_t)ADDRESS_SPACE_TABLE_MAPPING_LIMIT *
                    ADDRESS_SPACE_TABLE_PAGE_SIZE,
            USER_FRAME_BASE +
                (uint64_t)ADDRESS_SPACE_TABLE_MAPPING_LIMIT *
                    ADDRESS_SPACE_TABLE_PAGE_SIZE,
            rw
        ) != ADDRESS_SPACE_TABLE_STATUS_MAPPING_LIMIT ||
        tables.mapping_count != ADDRESS_SPACE_TABLE_MAPPING_LIMIT ||
        address_space_tables_validate(&tables) !=
            ADDRESS_SPACE_TABLE_STATUS_OK) {
        return false;
    }

    storage.entries[ADDRESS_SPACE_TABLE_PML4][0] ^= ENTRY_USER;
    mapping.physical_address = UINT64_MAX;
    mapping.permissions = UINT32_MAX;
    mapping.accessed = true;
    mapping.dirty = true;
    if (address_space_tables_validate(&tables) !=
            ADDRESS_SPACE_TABLE_STATUS_CORRUPTED ||
        address_space_tables_query(
            &tables, ADDRESS_SPACE_TABLE_USER_BASE, &mapping
        ) != ADDRESS_SPACE_TABLE_STATUS_CORRUPTED ||
        mapping.physical_address != 0U || mapping.permissions != 0U ||
        mapping.accessed || mapping.dirty) {
        return false;
    }

    address_space_tables_clear(&tables, &storage);
    if (!state_is_zero(&tables) || !storage_is_zero(&storage) ||
        build_current() != ADDRESS_SPACE_TABLE_STATUS_OK) {
        return false;
    }
    kernel_pml4[256] ^= ENTRY_USER;
    if (address_space_tables_validate(&tables) !=
            ADDRESS_SPACE_TABLE_STATUS_CORRUPTED) {
        return false;
    }
    kernel_pml4[256] ^= ENTRY_USER;
    return address_space_tables_validate(&tables) ==
        ADDRESS_SPACE_TABLE_STATUS_OK;
}

static bool self_test(void)
{
    reset_all();
    address_space_tables_clear(NULL, &storage);
    address_space_tables_clear(&tables, NULL);

    if (address_space_tables_validate(NULL) !=
            ADDRESS_SPACE_TABLE_STATUS_NULL_ARGUMENT ||
        address_space_tables_validate(&tables) !=
            ADDRESS_SPACE_TABLE_STATUS_NOT_INITIALIZED ||
        address_space_tables_query(&tables, 0U, NULL) !=
            ADDRESS_SPACE_TABLE_STATUS_NULL_ARGUMENT ||
        !malformed_build_rejections() ||
        !mapping_rejections_and_lifecycle() ||
        !mapping_limit_and_corruption()) {
        return false;
    }

    for (int status = ADDRESS_SPACE_TABLE_STATUS_OK;
         status <= ADDRESS_SPACE_TABLE_STATUS_CORRUPTED;
         ++status) {
        if (address_space_table_status_string(
                (enum address_space_table_status)status
            ) == NULL) {
            return false;
        }
    }
    return strcmp(
        address_space_table_status_string(
            (enum address_space_table_status)UINT32_MAX
        ),
        "unknown table status"
    ) == 0;
}

static uint64_t digest_mix(uint64_t digest, uint64_t value)
{
    digest ^= value;
    digest *= UINT64_C(1099511628211);
    return digest;
}

static uint64_t state_digest(void)
{
    uint64_t digest = UINT64_C(1469598103934665603);

    for (size_t page = 0U; page < ADDRESS_SPACE_TABLE_PAGE_COUNT; ++page) {
        for (size_t index = 0U;
             index < ADDRESS_SPACE_TABLE_ENTRY_COUNT;
             ++index) {
            digest = digest_mix(digest, storage.entries[page][index]);
        }
    }
    digest = digest_mix(digest, tables.initialized);
    digest = digest_mix(digest, tables.mapping_count);
    digest = digest_mix(digest, tables.physical_address_mask);
    for (size_t index = 0U;
         index < ADDRESS_SPACE_TABLE_PAGE_COUNT;
         ++index) {
        digest = digest_mix(
            digest, tables.table_physical_addresses[index]
        );
    }
    digest = digest_mix(digest, tables.kernel_pml4_physical_address);
    digest = digest_mix(digest, tables.kernel_low_pdpt_physical_address);
    return digest;
}

static bool parse_u64(const char *text, uint64_t *value)
{
    char *past;
    unsigned long long parsed;

    if (text == NULL || value == NULL) {
        return false;
    }
    parsed = strtoull(text, &past, 0);
    if (past == text || (*past != '\0' && *past != '\n')) {
        return false;
    }
    *value = (uint64_t)parsed;
    return true;
}

static void protocol_reply(
    enum address_space_table_status status,
    const struct address_space_table_mapping *mapping
)
{
    const uint64_t physical = mapping == NULL ? 0U :
        mapping->physical_address;
    const uint32_t permissions = mapping == NULL ? 0U :
        mapping->permissions;
    const unsigned int accessed = mapping != NULL && mapping->accessed ? 1U : 0U;
    const unsigned int dirty = mapping != NULL && mapping->dirty ? 1U : 0U;

    printf(
        "%u %016" PRIx64 " %016" PRIx64 " %08" PRIx32 " %u %u\n",
        (unsigned int)status,
        state_digest(),
        physical,
        permissions,
        accessed,
        dirty
    );
    fflush(stdout);
}

static int protocol(void)
{
    char line[256];

    reset_all();
    while (fgets(line, sizeof(line), stdin) != NULL) {
        char *command = strtok(line, " \t\r\n");
        enum address_space_table_status status;
        struct address_space_table_mapping mapping;
        uint64_t first;
        uint64_t second;
        uint64_t third;

        if (command == NULL) {
            continue;
        }
        if (strcmp(command, "quit") == 0) {
            return 0;
        }
        if (strcmp(command, "clear") == 0) {
            address_space_tables_clear(&tables, &storage);
            protocol_reply(ADDRESS_SPACE_TABLE_STATUS_OK, NULL);
            continue;
        }
        if (strcmp(command, "template-reset") == 0) {
            template_reset();
            protocol_reply(ADDRESS_SPACE_TABLE_STATUS_OK, NULL);
            continue;
        }
        if (strcmp(command, "build") == 0) {
            protocol_reply(build_current(), NULL);
            continue;
        }
        if (strcmp(command, "validate") == 0) {
            protocol_reply(address_space_tables_validate(&tables), NULL);
            continue;
        }

        if (!parse_u64(strtok(NULL, " \t\r\n"), &first)) {
            return 2;
        }
        if (strcmp(command, "query") == 0) {
            status = address_space_tables_query(&tables, first, &mapping);
            protocol_reply(status, &mapping);
            continue;
        }
        if (!parse_u64(strtok(NULL, " \t\r\n"), &second)) {
            return 2;
        }
        if (strcmp(command, "unmap") == 0) {
            protocol_reply(
                address_space_tables_unmap(&tables, first, second), NULL
            );
            continue;
        }
        if (strcmp(command, "repermission") == 0) {
            protocol_reply(
                address_space_tables_repermission(
                    &tables, first, (uint32_t)second
                ),
                NULL
            );
            continue;
        }
        if (strcmp(command, "table-set") == 0 ||
            strcmp(command, "template-set") == 0) {
            if (!parse_u64(strtok(NULL, " \t\r\n"), &third) ||
                first >= ADDRESS_SPACE_TABLE_PAGE_COUNT ||
                second >= ADDRESS_SPACE_TABLE_ENTRY_COUNT) {
                return 2;
            }
            if (strcmp(command, "table-set") == 0) {
                storage.entries[first][second] = third;
            } else if (first == 0U) {
                kernel_pml4[second] = third;
            } else if (first == 1U) {
                kernel_low_pdpt[second] = third;
            } else {
                return 2;
            }
            protocol_reply(ADDRESS_SPACE_TABLE_STATUS_OK, NULL);
            continue;
        }
        if (strcmp(command, "map") != 0 ||
            !parse_u64(strtok(NULL, " \t\r\n"), &third)) {
            return 2;
        }
        protocol_reply(
            address_space_tables_map(
                &tables, first, second, (uint32_t)third
            ),
            NULL
        );
    }
    return ferror(stdin) ? 3 : 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "selftest") == 0) {
        if (!self_test()) {
            fputs("address-space table self-test failed\n", stderr);
            return 1;
        }
        puts("address-space table self-test passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "protocol") == 0) {
        return protocol();
    }
    fprintf(stderr, "usage: %s selftest|protocol\n", argv[0]);
    return 2;
}
