/* SPDX-License-Identifier: GPL-3.0-only */
/* The production VFS must let a hidden journal transaction reach its retry. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/kernel/vfs.c"

static unsigned int calls;
static unsigned int stats;
static enum phipfs_status mutation_result = PHIPFS_STATUS_IO;

static enum phipfs_status hidden_stat(enum phipfs_volume volume,
    const char *path, struct phipfs_stat *result)
{
    (void)volume; (void)path; (void)result;
    ++stats;
    return PHIPFS_STATUS_IO;
}

static enum phipfs_status mutation(enum phipfs_volume volume, const char *path)
{
    assert(volume == PHIPFS_VOLUME_DATA && strcmp(path, "parent/file") == 0);
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
    struct vfs_backend_ops backend = {
        .stat_path = hidden_stat, .create = create, .truncate = truncate_file,
        .mkdir = mutation, .unlink = mutation, .rmdir = mutation,
        .link = pair, .rename = pair, .rename_replace = pair,
        .case_sensitive = true, .validates_mutation_paths = true,
    };
    mounts[PHIPFS_VOLUME_DATA].active = true;
    mounts[PHIPFS_VOLUME_DATA].backend = &backend;
    for (unsigned int attempt = 0U; attempt < 2U; ++attempt) {
        mutation_result = attempt == 0U ? PHIPFS_STATUS_IO : PHIPFS_STATUS_OK;
        assert(phipfs_create(PHIPFS_VOLUME_DATA, "parent/file") == mutation_result);
        assert(phipfs_truncate(PHIPFS_VOLUME_DATA, "parent/file", 7U) == mutation_result);
        assert(phipfs_mkdir(PHIPFS_VOLUME_DATA, "parent/file") == mutation_result);
        assert(phipfs_unlink(PHIPFS_VOLUME_DATA, "parent/file") == mutation_result);
        assert(phipfs_rmdir(PHIPFS_VOLUME_DATA, "parent/file") == mutation_result);
        assert(phipfs_link(PHIPFS_VOLUME_DATA, "parent/file", "other/target") == mutation_result);
        assert(phipfs_rename(PHIPFS_VOLUME_DATA, "parent/file", "other/target") == mutation_result);
        assert(phipfs_rename_replace(PHIPFS_VOLUME_DATA, "parent/file", "other/target") == mutation_result);
    }
    assert(calls == 16U && stats == 0U);
    assert(phipfs_unlink(PHIPFS_VOLUME_DATA, "../parent/file") == PHIPFS_STATUS_PATH);
    assert(phipfs_unlink(PHIPFS_VOLUME_DATA, ".") == PHIPFS_STATUS_ACCESS);
    assert(calls == 16U);
    mutation_result = PHIPFS_STATUS_NOT_DIRECTORY;
    assert(phipfs_create(PHIPFS_VOLUME_DATA, "parent/file") == PHIPFS_STATUS_NOT_DIRECTORY);
    mutation_result = PHIPFS_STATUS_NOT_FOUND;
    assert(phipfs_link(PHIPFS_VOLUME_DATA, "parent/file", "other/target") == PHIPFS_STATUS_NOT_FOUND);
    backend.validates_mutation_paths = false;
    assert(phipfs_create(PHIPFS_VOLUME_DATA, "parent/file") == PHIPFS_STATUS_IO);
    assert(calls == 18U && stats == 1U);
    assert(mounts[PHIPFS_VOLUME_DATA].references == 0U);
    for (size_t index = 0U; index < VFS_MAX_VNODES; ++index) assert(!vnodes[index].active);
    puts("VFS journal mutation retries, backend errors, path bounds and vnode census: PASS");
    return 0;
}
