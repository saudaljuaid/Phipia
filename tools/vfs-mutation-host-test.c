/* SPDX-License-Identifier: GPL-3.0-only */
/* The production VFS must let a hidden journal transaction reach its retry. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/kernel/vfs.c"

static unsigned int calls;
static unsigned int stats;
static enum phipfs_status mutation_result = PHIPFS_STATUS_IO;
static const char *expected_path = "parent/file";
static bool stat_succeeds;

static enum phipfs_status hidden_stat(enum phipfs_volume volume,
    const char *path, struct phipfs_stat *result)
{
    (void)volume;
    ++stats;
    if (stat_succeeds) {
        assert(strcmp(path, expected_path) == 0);
        memset(result, 0, sizeof(*result));
        result->object_id = 101U;
        return PHIPFS_STATUS_OK;
    }
    return PHIPFS_STATUS_IO;
}

static enum phipfs_status mutation(enum phipfs_volume volume, const char *path)
{
    assert(volume == PHIPFS_VOLUME_DATA && strcmp(path, expected_path) == 0);
    ++calls;
    return mutation_result;
}

static enum phipfs_status pair(enum phipfs_volume volume, const char *from, const char *to)
{
    assert(strcmp(to, "other/target") == 0);
    return mutation(volume, from);
}

static enum phipfs_status create(enum phipfs_volume volume, const char *path, uint16_t mode)
{
    assert(mode == 0644U);
    return mutation(volume, path);
}

static enum phipfs_status truncate_file(enum phipfs_volume volume, const char *path, uint64_t size)
{
    assert(size == 7U);
    return mutation(volume, path);
}

int main(void)
{
    for (size_t index = 0U; index < VFS_VNODE_BUCKETS; ++index) vnode_buckets[index] = VFS_NO_INDEX;
    struct vfs_backend_ops backend = {
        .stat_path = hidden_stat, .create = create, .truncate = truncate_file,
        .mkdir = mutation, .unlink = mutation, .rmdir = mutation,
        .link = pair, .rename = pair, .rename_replace = pair,
        .case_sensitive = true, .validates_mutation_paths = true,
        .remove = mutation, .chmod = create,
    };
    mounts[PHIPFS_VOLUME_DATA].active = true;
    mounts[PHIPFS_VOLUME_DATA].backend = &backend;
    for (unsigned int attempt = 0U; attempt < 2U; ++attempt) {
        mutation_result = attempt == 0U ? PHIPFS_STATUS_IO : PHIPFS_STATUS_OK;
        assert(phipfs_create(PHIPFS_VOLUME_DATA, "parent/file") == mutation_result);
        assert(phipfs_truncate(PHIPFS_VOLUME_DATA, "parent/file", 7U) == mutation_result);
        assert(phipfs_mkdir(PHIPFS_VOLUME_DATA, "parent/file") == mutation_result);
        assert(phipfs_unlink(PHIPFS_VOLUME_DATA, "parent/file") == mutation_result);
        assert(phipfs_remove(PHIPFS_VOLUME_DATA, "parent/file") == mutation_result);
        assert(phipfs_rmdir(PHIPFS_VOLUME_DATA, "parent/file") == mutation_result);
        assert(phipfs_link(PHIPFS_VOLUME_DATA, "parent/file", "other/target") == mutation_result);
        assert(phipfs_rename(PHIPFS_VOLUME_DATA, "parent/file", "other/target") == mutation_result);
        assert(phipfs_rename_replace(PHIPFS_VOLUME_DATA, "parent/file", "other/target") == mutation_result);
    }
    assert(calls == 18U && stats == 0U);
    assert(phipfs_unlink(PHIPFS_VOLUME_DATA, "../parent/file") == PHIPFS_STATUS_PATH);
    assert(phipfs_unlink(PHIPFS_VOLUME_DATA, ".") == PHIPFS_STATUS_ACCESS);
    assert(calls == 18U);
    mutation_result = PHIPFS_STATUS_NOT_DIRECTORY;
    assert(phipfs_create(PHIPFS_VOLUME_DATA, "parent/file") == PHIPFS_STATUS_NOT_DIRECTORY);
    mutation_result = PHIPFS_STATUS_NOT_FOUND;
    assert(phipfs_link(PHIPFS_VOLUME_DATA, "parent/file", "other/target") == PHIPFS_STATUS_NOT_FOUND);
    backend.validates_mutation_paths = false;
    assert(phipfs_create(PHIPFS_VOLUME_DATA, "parent/file") == PHIPFS_STATUS_IO);
    assert(calls == 20U && stats == 1U);
    backend.validates_mutation_paths = true;
    for (size_t index = 0U; index < 3U; ++index) {
        const char *paths[] = {"parent/link/../file", "parent/missing/../file", "parent/file/./child"};
        expected_path = paths[index];
        mutation_result = index == 2U ? PHIPFS_STATUS_NOT_DIRECTORY : PHIPFS_STATUS_NOT_FOUND;
        assert(phipfs_create(PHIPFS_VOLUME_DATA, expected_path) == mutation_result);
        assert(phipfs_unlink(PHIPFS_VOLUME_DATA, expected_path) == mutation_result);
    }
    stat_succeeds = true;
    expected_path = "parent/link/../file";
    struct phipfs_stat metadata;
    assert(phipfs_stat_path(PHIPFS_VOLUME_DATA, expected_path, &metadata) == PHIPFS_STATUS_OK);
    assert(metadata.object_id == 101U && stats == 2U);
    expected_path = ".";
    mutation_result = PHIPFS_STATUS_OK;
    assert(phipfs_chmod(PHIPFS_VOLUME_DATA, expected_path, 0644U) == PHIPFS_STATUS_OK);
    assert(phipfs_create(PHIPFS_VOLUME_DATA, expected_path) == PHIPFS_STATUS_ACCESS);
    expected_path = "parent/caf\xc3\xa9";
    assert(phipfs_create(PHIPFS_VOLUME_DATA, expected_path) == PHIPFS_STATUS_OK);
    assert(mounts[PHIPFS_VOLUME_DATA].references == 0U);
    for (size_t index = 0U; index < VFS_MAX_VNODES; ++index) assert(!vnodes[index].active);
    puts("VFS journal mutation retries, backend errors, path bounds and vnode census: PASS");
    return 0;
}
