/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <seneri/boot.h>

#define ACPI_RSDP_V1_SIZE 20U
#define ACPI_RSDP_V2_SIZE 36U

static bool range_fits(size_t offset, size_t length, size_t total)
{
    return offset <= total && length <= total - offset;
}

static size_t align_tag_size(size_t size)
{
    const size_t mask = MULTIBOOT2_TAG_ALIGNMENT - 1U;

    return (size + mask) & ~mask;
}

static void boot_context_reset(struct boot_context *context)
{
    context->information_start = 0;
    context->information_end = 0;
    context->memory_map = NULL;
    context->acpi_old = NULL;
    context->acpi_new = NULL;
    context->boot_loader_name = NULL;
    context->command_line = NULL;
    context->boot_loader_name_length = 0;
    context->command_line_length = 0;
    context->memory_map_entry_count = 0;
    context->reported_usable_bytes = 0;
    context->highest_reported_address = 0;
}

static bool string_length_within(
    const struct multiboot2_string_tag *tag,
    size_t tag_size,
    size_t *string_length
)
{
    const size_t header_size = sizeof(tag->tag);

    if (tag_size <= header_size) {
        return false;
    }

    for (size_t index = header_size; index < tag_size; ++index) {
        const uint8_t *bytes = (const uint8_t *)(const void *)tag;

        if (bytes[index] == 0U) {
            *string_length = index - header_size;
            return true;
        }
    }

    return false;
}

static enum boot_status validate_memory_map(
    const struct multiboot2_memory_map_tag *map,
    struct boot_context *context
)
{
    const size_t map_header_size = sizeof(*map);
    size_t payload_size;
    size_t entry_count;

    if (map->tag.size < map_header_size) {
        return BOOT_STATUS_MEMORY_MAP_TOO_SMALL;
    }

    if (map->entry_size < sizeof(struct multiboot2_memory_map_entry)) {
        return BOOT_STATUS_MEMORY_ENTRY_TOO_SMALL;
    }

    if (map->entry_version != 0U) {
        return BOOT_STATUS_MEMORY_ENTRY_VERSION;
    }

    payload_size = map->tag.size - map_header_size;

    if (payload_size % map->entry_size != 0U) {
        return BOOT_STATUS_MEMORY_MAP_REMAINDER;
    }

    entry_count = payload_size / map->entry_size;
    context->memory_map = map;
    context->memory_map_entry_count = entry_count;

    for (size_t index = 0; index < entry_count; ++index) {
        struct boot_memory_region region;
        uint64_t region_end;

        if (!boot_memory_region_at(context, index, &region)) {
            return BOOT_STATUS_TRUNCATED_TAG;
        }

        if (region.length > UINT64_MAX - region.base_address) {
            return BOOT_STATUS_MEMORY_REGION_OVERFLOW;
        }

        region_end = region.base_address + region.length;

        if (region_end > context->highest_reported_address) {
            context->highest_reported_address = region_end;
        }

        if (region.type == MULTIBOOT2_MEMORY_AVAILABLE) {
            if (region.length > UINT64_MAX - context->reported_usable_bytes) {
                context->reported_usable_bytes = UINT64_MAX;
            } else {
                context->reported_usable_bytes += region.length;
            }
        }
    }

    return BOOT_STATUS_OK;
}

enum boot_status boot_context_parse(
    uint32_t magic,
    uintptr_t information_address,
    struct boot_context *context
)
{
    const struct multiboot2_information_header *information;
    const uint8_t *bytes;
    size_t total_size;
    size_t offset;
    bool found_end = false;

    if (context == NULL) {
        return BOOT_STATUS_NULL_CONTEXT;
    }

    boot_context_reset(context);

    if (magic != MULTIBOOT2_BOOT_MAGIC) {
        return BOOT_STATUS_BAD_MAGIC;
    }

    if (information_address == 0U) {
        return BOOT_STATUS_NULL_INFORMATION;
    }

    if ((information_address & (MULTIBOOT2_TAG_ALIGNMENT - 1U)) != 0U) {
        return BOOT_STATUS_MISALIGNED_INFORMATION;
    }

    if ((uint64_t)information_address >
        SENERI_EARLY_PHYSICAL_LIMIT - sizeof(*information)) {
        return BOOT_STATUS_INFORMATION_TOO_LARGE;
    }

    information = (const struct multiboot2_information_header *)information_address;
    total_size = information->total_size;

    if (total_size < sizeof(*information) + sizeof(struct multiboot2_tag)) {
        return BOOT_STATUS_INFORMATION_TOO_SMALL;
    }

    if (total_size > MULTIBOOT2_MAX_INFORMATION_SIZE) {
        return BOOT_STATUS_INFORMATION_TOO_LARGE;
    }

    if ((total_size & (MULTIBOOT2_TAG_ALIGNMENT - 1U)) != 0U) {
        return BOOT_STATUS_INFORMATION_SIZE_MISALIGNED;
    }

    if (information_address > UINTPTR_MAX - total_size) {
        return BOOT_STATUS_INFORMATION_OVERFLOW;
    }

    if ((uint64_t)information_address + total_size > SENERI_EARLY_PHYSICAL_LIMIT) {
        return BOOT_STATUS_INFORMATION_TOO_LARGE;
    }

    if (information->reserved != 0U) {
        return BOOT_STATUS_RESERVED_FIELD_NONZERO;
    }

    context->information_start = information_address;
    context->information_end = information_address + total_size;
    bytes = (const uint8_t *)information_address;
    offset = sizeof(*information);

    while (offset < total_size) {
        const struct multiboot2_tag *tag;
        size_t aligned_size;

        if (!range_fits(offset, sizeof(*tag), total_size)) {
            return BOOT_STATUS_TRUNCATED_TAG;
        }

        tag = (const struct multiboot2_tag *)(const void *)(bytes + offset);

        if (tag->size < sizeof(*tag)) {
            return BOOT_STATUS_TAG_TOO_SMALL;
        }

        if (!range_fits(offset, tag->size, total_size)) {
            return BOOT_STATUS_TAG_OVERFLOW;
        }

        if (tag->type == MULTIBOOT2_TAG_END) {
            if (tag->size != sizeof(*tag) || offset + tag->size != total_size) {
                return BOOT_STATUS_BAD_END_TAG;
            }

            found_end = true;
            break;
        }

        if (tag->type == MULTIBOOT2_TAG_MEMORY_MAP) {
            enum boot_status status;

            if (context->memory_map != NULL) {
                return BOOT_STATUS_DUPLICATE_MEMORY_MAP;
            }

            status = validate_memory_map(
                (const struct multiboot2_memory_map_tag *)(const void *)tag,
                context
            );

            if (status != BOOT_STATUS_OK) {
                return status;
            }
        } else if (tag->type == MULTIBOOT2_TAG_MODULE) {
            /* Modules need explicit lifetime ownership before they are safe. */
            return BOOT_STATUS_UNSUPPORTED_MODULE;
        } else if (tag->type == MULTIBOOT2_TAG_ACPI_OLD ||
                   tag->type == MULTIBOOT2_TAG_ACPI_NEW) {
            const struct multiboot2_acpi_tag *acpi_tag =
                (const struct multiboot2_acpi_tag *)(const void *)tag;
            const size_t minimum_payload = tag->type == MULTIBOOT2_TAG_ACPI_NEW
                ? ACPI_RSDP_V2_SIZE
                : ACPI_RSDP_V1_SIZE;
            const struct multiboot2_acpi_tag **destination =
                tag->type == MULTIBOOT2_TAG_ACPI_NEW
                    ? &context->acpi_new
                    : &context->acpi_old;

            if (tag->size < sizeof(*acpi_tag) + minimum_payload) {
                return BOOT_STATUS_ACPI_TAG_TOO_SMALL;
            }

            if (*destination != NULL) {
                return BOOT_STATUS_DUPLICATE_ACPI_TAG;
            }

            *destination = acpi_tag;
        } else if (tag->type == MULTIBOOT2_TAG_BOOT_LOADER_NAME ||
                   tag->type == MULTIBOOT2_TAG_COMMAND_LINE) {
            const struct multiboot2_string_tag *string_tag =
                (const struct multiboot2_string_tag *)(const void *)tag;
            size_t string_length;

            if (!string_length_within(string_tag, tag->size, &string_length)) {
                return BOOT_STATUS_STRING_NOT_TERMINATED;
            }

            if (tag->type == MULTIBOOT2_TAG_BOOT_LOADER_NAME) {
                context->boot_loader_name = string_tag->string;
                context->boot_loader_name_length = string_length;
            } else {
                context->command_line = string_tag->string;
                context->command_line_length = string_length;
            }
        }

        aligned_size = align_tag_size(tag->size);

        if (aligned_size < tag->size || !range_fits(offset, aligned_size, total_size)) {
            return BOOT_STATUS_TAG_OVERFLOW;
        }

        offset += aligned_size;
    }

    if (!found_end) {
        return BOOT_STATUS_MISSING_END_TAG;
    }

    if (context->memory_map == NULL) {
        return BOOT_STATUS_MISSING_MEMORY_MAP;
    }

    return BOOT_STATUS_OK;
}

bool boot_memory_region_at(
    const struct boot_context *context,
    size_t index,
    struct boot_memory_region *region
)
{
    const uint8_t *entry_address;
    const struct multiboot2_memory_map_entry *entry;

    if (context == NULL || region == NULL || context->memory_map == NULL ||
        index >= context->memory_map_entry_count) {
        return false;
    }

    entry_address = context->memory_map->entries +
        index * context->memory_map->entry_size;
    entry = (const struct multiboot2_memory_map_entry *)(const void *)entry_address;

    region->base_address = entry->base_address;
    region->length = entry->length;
    region->type = entry->type;
    return true;
}

const char *boot_status_string(enum boot_status status)
{
    switch (status) {
    case BOOT_STATUS_OK:
        return "ok";
    case BOOT_STATUS_NULL_CONTEXT:
        return "null boot context";
    case BOOT_STATUS_BAD_MAGIC:
        return "invalid Multiboot2 magic";
    case BOOT_STATUS_NULL_INFORMATION:
        return "null Multiboot2 information address";
    case BOOT_STATUS_MISALIGNED_INFORMATION:
        return "misaligned Multiboot2 information";
    case BOOT_STATUS_INFORMATION_TOO_SMALL:
        return "Multiboot2 information is too small";
    case BOOT_STATUS_INFORMATION_TOO_LARGE:
        return "Multiboot2 information is outside the early map";
    case BOOT_STATUS_INFORMATION_SIZE_MISALIGNED:
        return "Multiboot2 information size is misaligned";
    case BOOT_STATUS_INFORMATION_OVERFLOW:
        return "Multiboot2 information address overflows";
    case BOOT_STATUS_RESERVED_FIELD_NONZERO:
        return "Multiboot2 reserved field is nonzero";
    case BOOT_STATUS_TRUNCATED_TAG:
        return "truncated Multiboot2 tag";
    case BOOT_STATUS_TAG_TOO_SMALL:
        return "Multiboot2 tag is too small";
    case BOOT_STATUS_TAG_OVERFLOW:
        return "Multiboot2 tag exceeds its container";
    case BOOT_STATUS_DUPLICATE_MEMORY_MAP:
        return "duplicate Multiboot2 memory map";
    case BOOT_STATUS_MEMORY_MAP_TOO_SMALL:
        return "Multiboot2 memory map is too small";
    case BOOT_STATUS_MEMORY_ENTRY_TOO_SMALL:
        return "Multiboot2 memory entry is too small";
    case BOOT_STATUS_MEMORY_ENTRY_VERSION:
        return "unsupported Multiboot2 memory entry version";
    case BOOT_STATUS_MEMORY_MAP_REMAINDER:
        return "Multiboot2 memory map has a partial entry";
    case BOOT_STATUS_MEMORY_REGION_OVERFLOW:
        return "Multiboot2 memory region overflows";
    case BOOT_STATUS_UNSUPPORTED_MODULE:
        return "Multiboot2 modules are not supported yet";
    case BOOT_STATUS_STRING_NOT_TERMINATED:
        return "Multiboot2 string is not terminated";
    case BOOT_STATUS_ACPI_TAG_TOO_SMALL:
        return "Multiboot2 ACPI tag is too small";
    case BOOT_STATUS_DUPLICATE_ACPI_TAG:
        return "duplicate Multiboot2 ACPI tag";
    case BOOT_STATUS_BAD_END_TAG:
        return "invalid Multiboot2 end tag";
    case BOOT_STATUS_MISSING_END_TAG:
        return "missing Multiboot2 end tag";
    case BOOT_STATUS_MISSING_MEMORY_MAP:
        return "missing Multiboot2 memory map";
    default:
        return "unknown boot status";
    }
}
