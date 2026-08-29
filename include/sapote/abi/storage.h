/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_ABI_STORAGE_H
#define SAPOTE_ABI_STORAGE_H

#include <sapote/abi/base.h>

#define SAPOTE_PATH_MAX 127U
#define SAPOTE_DIRECTORY_NAME_MAX 12U

enum sapote_volume {
    SAPOTE_VOLUME_SYSTEM = 1,
    SAPOTE_VOLUME_DATA = 2
};

enum sapote_open_flags {
    SAPOTE_OPEN_READ = UINT32_C(1) << 0,
    SAPOTE_OPEN_WRITE = UINT32_C(1) << 1,
    SAPOTE_OPEN_CREATE = UINT32_C(1) << 2,
    SAPOTE_OPEN_TRUNCATE = UINT32_C(1) << 3
};

#define SAPOTE_OPEN_FLAGS_V1 (SAPOTE_OPEN_READ | SAPOTE_OPEN_WRITE | \
    SAPOTE_OPEN_CREATE | SAPOTE_OPEN_TRUNCATE)

enum sapote_seek_origin {
    SAPOTE_SEEK_START = 0,
    SAPOTE_SEEK_CURRENT = 1,
    SAPOTE_SEEK_END = 2
};

struct sapote_path {
    uint64_t address;
    uint32_t length;
    uint16_t volume;
    uint16_t reserved;
} __attribute__((packed));

struct sapote_file_open_request {
    uint32_t size;
    uint32_t version;
    struct sapote_path path;
    uint32_t flags;
    uint32_t reserved;
} __attribute__((packed));

struct sapote_io_request {
    uint32_t size;
    uint32_t version;
    sapote_handle_t handle;
    uint64_t buffer;
    uint64_t offset;
    uint32_t length;
    uint32_t flags;
} __attribute__((packed));

struct sapote_seek_request {
    uint32_t size;
    uint32_t version;
    sapote_handle_t handle;
    int64_t offset;
    uint32_t origin;
    uint32_t reserved;
} __attribute__((packed));

struct sapote_path_stat {
    uint32_t size;
    uint32_t version;
    uint64_t byte_length;
    uint32_t attributes;
    uint32_t reserved;
} __attribute__((packed));

enum sapote_path_attributes {
    SAPOTE_PATH_DIRECTORY = UINT32_C(1) << 0,
    SAPOTE_PATH_READ_ONLY = UINT32_C(1) << 1
};

struct sapote_directory_entry {
    uint32_t size;
    uint32_t version;
    uint64_t byte_length;
    uint32_t attributes;
    uint16_t name_length;
    uint16_t reserved;
    uint8_t name[SAPOTE_DIRECTORY_NAME_MAX];
    uint32_t reserved_tail;
} __attribute__((packed));

struct sapote_rename_request {
    uint32_t size;
    uint32_t version;
    struct sapote_path source;
    struct sapote_path destination;
    uint32_t flags;
    uint32_t reserved;
} __attribute__((packed));

struct sapote_volume_space {
    uint32_t size;
    uint32_t version;
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint32_t cluster_bytes;
    uint32_t reserved;
} __attribute__((packed));

_Static_assert(sizeof(struct sapote_path) == 16U,
    "Sapote path ABI changed");
_Static_assert(sizeof(struct sapote_file_open_request) == 32U,
    "Sapote file-open ABI changed");
_Static_assert(sizeof(struct sapote_io_request) == 40U,
    "Sapote I/O ABI changed");
_Static_assert(sizeof(struct sapote_seek_request) == 32U,
    "Sapote seek ABI changed");
_Static_assert(sizeof(struct sapote_path_stat) == 24U,
    "Sapote stat ABI changed");
_Static_assert(sizeof(struct sapote_directory_entry) == 40U,
    "Sapote directory-entry ABI changed");
_Static_assert(sizeof(struct sapote_rename_request) == 48U,
    "Sapote rename ABI changed");
_Static_assert(sizeof(struct sapote_volume_space) == 32U,
    "Sapote volume-space ABI changed");

#endif
