/* SPDX-License-Identifier: GPL-3.0-only */
/* Exercise the production C backend with explicit Rust/NVMe refusal stubs. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/kernel/ext4_fs.c"

static unsigned opens;
static unsigned closes;
static unsigned truncates;
static unsigned stats;
static unsigned refusals;
static bool pending;
static uint64_t disk_size = 8192U;
static int32_t permanent_status = PHIPIA_EXT4_STATUS_OK;

enum nvme_status nvme_volume_open(struct nvme_volume_session *session,
    uint32_t controller_index, bool writable)
{
    assert(!session->active);
    memset(session, 0, sizeof(*session));
    session->namespace_blocks = 32768U;
    session->logical_block_bytes = 4096U;
    session->controller_index = controller_index;
    session->writable = writable;
    session->active = true;
    ++opens;
    return NVME_STATUS_OK;
}

enum nvme_status nvme_volume_close(struct nvme_volume_session *session)
{
    assert(session->active);
    session->active = false;
    ++closes;
    return NVME_STATUS_OK;
}

int32_t phipia_ext4_truncate_probe(uintptr_t mounted, const uint8_t *path,
    size_t path_bytes, uint64_t size)
{
    assert(mounted == 1U && path_bytes == 4U && memcmp(path, "file", 4U) == 0);
    assert(ext4_mounts[PHIPFS_VOLUME_DATA].session.writable);
    ++truncates;
    if (permanent_status != PHIPIA_EXT4_STATUS_OK) {
        return permanent_status;
    }
    if (refusals != 0U) {
        --refusals;
        pending = true;
        return PHIPIA_EXT4_STATUS_IO;
    }
    pending = false;
    disk_size = size;
    return PHIPIA_EXT4_STATUS_OK;
}

int32_t phipia_ext4_stat(uintptr_t mounted, const uint8_t *path,
    size_t path_bytes, struct phipia_ext4_metadata *metadata)
{
    assert(mounted == 1U && path_bytes == 4U && memcmp(path, "file", 4U) == 0);
    assert(!ext4_mounts[PHIPFS_VOLUME_DATA].session.writable);
    ++stats;
    if (pending) {
        return PHIPIA_EXT4_STATUS_IO;
    }
    memset(metadata, 0, sizeof(*metadata));
    metadata->inode = 42U;
    metadata->size = disk_size;
    metadata->file_type = PHIPIA_EXT4_FILE_REGULAR;
    return PHIPIA_EXT4_STATUS_OK;
}

int main(void)
{
    phipfs_handle first;
    phipfs_handle second;
    struct ext4_handle_state *state;
    ext4_backend_initialize();
    ext4_mounts[PHIPFS_VOLUME_DATA].active = true;
    ext4_mounts[PHIPFS_VOLUME_DATA].healthy = true;
    ext4_mounts[PHIPFS_VOLUME_DATA].generation = 1U;
    ext4_mounts[PHIPFS_VOLUME_DATA].rust_mount = 1U;
    assert(allocate_handle(PHIPFS_VOLUME_DATA, "file", 42U, disk_size,
        PHIPFS_ACCESS_READ, false, &first) == PHIPFS_STATUS_OK);
    assert(allocate_handle(PHIPFS_VOLUME_DATA, "file", 42U, disk_size,
        PHIPFS_ACCESS_WRITE, false, &second) == PHIPFS_STATUS_OK);
    refusals = 2U;
    for (unsigned attempt = 0U; attempt < 2U; ++attempt) {
        assert(ext4_backend_truncate(PHIPFS_VOLUME_DATA, "file", 101U) == PHIPFS_STATUS_IO);
        assert(handle_state(first, &state) == PHIPFS_STATUS_OK && state->size == 8192U);
        assert(opens == closes && !ext4_mounts[PHIPFS_VOLUME_DATA].operation_active);
    }
    assert(ext4_backend_truncate(PHIPFS_VOLUME_DATA, "file", 101U) == PHIPFS_STATUS_OK);
    assert(truncates == 3U && stats == 1U);
    assert(handle_state(first, &state) == PHIPFS_STATUS_OK && state->size == 101U);
    assert(handle_state(second, &state) == PHIPFS_STATUS_OK && state->size == 101U);
    permanent_status = PHIPIA_EXT4_STATUS_FULL;
    assert(ext4_backend_truncate(PHIPFS_VOLUME_DATA, "file", 100U) == PHIPFS_STATUS_FULL);
    permanent_status = PHIPIA_EXT4_STATUS_READ_ONLY;
    assert(ext4_backend_truncate(PHIPFS_VOLUME_DATA, "file", 100U) == PHIPFS_STATUS_READ_ONLY);
    assert(handle_state(first, &state) == PHIPFS_STATUS_OK && state->size == 101U);
    assert(ext4_backend_close(first) == PHIPFS_STATUS_OK);
    assert(ext4_backend_close(second) == PHIPFS_STATUS_OK);
    assert(handle_state(first, &state) == PHIPFS_STATUS_STALE_HANDLE);
    assert(opens == closes && !ext4_mounts[PHIPFS_VOLUME_DATA].session.active);
    puts("ext4 VFS truncate retries, shared handle sizes, errors and leases: PASS");
    return 0;
}
