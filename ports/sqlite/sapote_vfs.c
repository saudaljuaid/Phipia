/* SPDX-License-Identifier: GPL-3.0-only */
/* SQLite VFS over the public Sapote SDK. WAL and cross-process locks are not exposed. */

#include "sqlite3.h"

#include <errno.h>
#include <fcntl.h>
#include <sapote/runtime.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SAPOTE_SQLITE_OPEN_MAX 16

struct sapote_sqlite_file {
    sqlite3_file base;
    int descriptor;
    int lock_level;
    int delete_on_close;
    char path[FILENAME_MAX];
};

static struct sapote_sqlite_file *open_files[SAPOTE_SQLITE_OPEN_MAX];

static int short_name(const char *name)
{
    size_t base = 0U;
    size_t extension = 0U;
    int in_extension = 0;

    if (name == NULL || *name == '\0') return 0;
    for (; *name != '\0'; ++name) {
        const unsigned char value = (unsigned char)*name;
        if (value == '.') {
            if (in_extension || base == 0U) return 0;
            in_extension = 1;
        } else if (!((value >= 'A' && value <= 'Z') ||
                (value >= 'a' && value <= 'z') ||
                (value >= '0' && value <= '9') || value == '_')) {
            return 0;
        } else if (in_extension) {
            if (++extension > 3U) return 0;
        } else if (++base > 8U) {
            return 0;
        }
    }
    return base != 0U && (!in_extension || extension != 0U);
}

static int map_path(const char *logical, char output[FILENAME_MAX])
{
    static const char suffix[] = "-journal";
    size_t length;

    if (logical == NULL) logical = "TEMP.DB";
    length = strlen(logical);
    if (length >= sizeof(suffix) - 1U &&
        strcmp(logical + length - (sizeof(suffix) - 1U), suffix) == 0) {
        const size_t stem_length = length - (sizeof(suffix) - 1U);
        const char *dot = memchr(logical, '.', stem_length);
        const size_t base_length = dot == NULL ? stem_length :
            (size_t)(dot - logical);

        if (base_length == 0U || base_length > 8U) return SQLITE_CANTOPEN;
        (void)memcpy(output, logical, base_length);
        (void)memcpy(output + base_length, ".JRN", 5U);
        return short_name(output) ? SQLITE_OK : SQLITE_CANTOPEN;
    }
    if (length >= FILENAME_MAX || !short_name(logical)) return SQLITE_CANTOPEN;
    (void)memcpy(output, logical, length + 1U);
    return SQLITE_OK;
}

static int register_file(struct sapote_sqlite_file *file)
{
    for (size_t index = 0U; index < SAPOTE_SQLITE_OPEN_MAX; ++index) {
        if (open_files[index] == NULL) {
            open_files[index] = file;
            return SQLITE_OK;
        }
    }
    return SQLITE_CANTOPEN;
}

static void unregister_file(struct sapote_sqlite_file *file)
{
    for (size_t index = 0U; index < SAPOTE_SQLITE_OPEN_MAX; ++index) {
        if (open_files[index] == file) open_files[index] = NULL;
    }
}

static int sapote_close(sqlite3_file *base)
{
    struct sapote_sqlite_file *file = (struct sapote_sqlite_file *)base;
    const int status = close(file->descriptor);

    unregister_file(file);
    if (file->delete_on_close) (void)unlink(file->path);
    file->base.pMethods = NULL;
    return status == 0 ? SQLITE_OK : SQLITE_IOERR_CLOSE;
}

static int sapote_read(sqlite3_file *base, void *buffer, int amount,
    sqlite3_int64 offset)
{
    struct sapote_sqlite_file *file = (struct sapote_sqlite_file *)base;
    unsigned char *bytes = buffer;
    int completed = 0;

    if (amount < 0 || offset < 0 || lseek(file->descriptor, offset, SEEK_SET) < 0) {
        return SQLITE_IOERR_SEEK;
    }
    while (completed < amount) {
        const ssize_t result = read(file->descriptor, bytes + completed,
            (size_t)(amount - completed));
        if (result < 0) return SQLITE_IOERR_READ;
        if (result == 0) {
            (void)memset(bytes + completed, 0, (size_t)(amount - completed));
            return SQLITE_IOERR_SHORT_READ;
        }
        completed += (int)result;
    }
    return SQLITE_OK;
}

static int sapote_write(sqlite3_file *base, const void *buffer, int amount,
    sqlite3_int64 offset)
{
    struct sapote_sqlite_file *file = (struct sapote_sqlite_file *)base;
    const unsigned char *bytes = buffer;
    int completed = 0;

    if (amount < 0 || offset < 0 || lseek(file->descriptor, offset, SEEK_SET) < 0) {
        return SQLITE_IOERR_SEEK;
    }
    while (completed < amount) {
        const ssize_t result = write(file->descriptor, bytes + completed,
            (size_t)(amount - completed));
        if (result <= 0) return errno == ENOSPC ? SQLITE_FULL : SQLITE_IOERR_WRITE;
        completed += (int)result;
    }
    return SQLITE_OK;
}

static int sapote_truncate(sqlite3_file *base, sqlite3_int64 size)
{
    const struct sapote_sqlite_file *file =
        (const struct sapote_sqlite_file *)base;
    return size >= 0 && ftruncate(file->descriptor, size) == 0 ?
        SQLITE_OK : SQLITE_IOERR_TRUNCATE;
}

static int sapote_sync(sqlite3_file *base, int flags)
{
    const struct sapote_sqlite_file *file =
        (const struct sapote_sqlite_file *)base;
    (void)flags;
    return fsync(file->descriptor) == 0 ? SQLITE_OK : SQLITE_IOERR_FSYNC;
}

static int sapote_file_size(sqlite3_file *base, sqlite3_int64 *size)
{
    const struct sapote_sqlite_file *file =
        (const struct sapote_sqlite_file *)base;
    const off_t current = lseek(file->descriptor, 0, SEEK_CUR);
    const off_t end = lseek(file->descriptor, 0, SEEK_END);

    if (current < 0 || end < 0 || lseek(file->descriptor, current, SEEK_SET) < 0) {
        return SQLITE_IOERR_FSTAT;
    }
    *size = end;
    return SQLITE_OK;
}

static int lock_conflict(const struct sapote_sqlite_file *file, int requested)
{
    for (size_t index = 0U; index < SAPOTE_SQLITE_OPEN_MAX; ++index) {
        const struct sapote_sqlite_file *other = open_files[index];
        if (other == NULL || other == file || strcmp(other->path, file->path) != 0) {
            continue;
        }
        if (requested == SQLITE_LOCK_SHARED) {
            if (other->lock_level >= SQLITE_LOCK_PENDING) return 1;
        } else if (requested == SQLITE_LOCK_RESERVED) {
            if (other->lock_level >= SQLITE_LOCK_RESERVED) return 1;
        } else if (requested == SQLITE_LOCK_PENDING) {
            if (other->lock_level >= SQLITE_LOCK_PENDING) return 1;
        } else if (requested == SQLITE_LOCK_EXCLUSIVE &&
            other->lock_level != SQLITE_LOCK_NONE) {
            return 1;
        }
    }
    return 0;
}

static int sapote_lock(sqlite3_file *base, int requested)
{
    struct sapote_sqlite_file *file = (struct sapote_sqlite_file *)base;
    if (requested <= file->lock_level) return SQLITE_OK;
    if (lock_conflict(file, requested)) return SQLITE_BUSY;
    file->lock_level = requested;
    return SQLITE_OK;
}

static int sapote_unlock(sqlite3_file *base, int requested)
{
    struct sapote_sqlite_file *file = (struct sapote_sqlite_file *)base;
    if (requested < SQLITE_LOCK_NONE || requested > file->lock_level) {
        return SQLITE_IOERR_UNLOCK;
    }
    file->lock_level = requested;
    return SQLITE_OK;
}

static int sapote_check_reserved(sqlite3_file *base, int *result)
{
    const struct sapote_sqlite_file *file =
        (const struct sapote_sqlite_file *)base;
    *result = 0;
    for (size_t index = 0U; index < SAPOTE_SQLITE_OPEN_MAX; ++index) {
        const struct sapote_sqlite_file *other = open_files[index];
        if (other != NULL && strcmp(other->path, file->path) == 0 &&
            other->lock_level >= SQLITE_LOCK_RESERVED) {
            *result = 1;
            break;
        }
    }
    return SQLITE_OK;
}

static int sapote_file_control(sqlite3_file *base, int operation, void *argument)
{
    (void)base;
    (void)operation;
    (void)argument;
    return SQLITE_NOTFOUND;
}

static int sapote_sector_size(sqlite3_file *base)
{
    (void)base;
    return 512;
}

static int sapote_device_characteristics(sqlite3_file *base)
{
    (void)base;
    return 0;
}

static const sqlite3_io_methods sapote_io = {
    .iVersion = 1,
    .xClose = sapote_close,
    .xRead = sapote_read,
    .xWrite = sapote_write,
    .xTruncate = sapote_truncate,
    .xSync = sapote_sync,
    .xFileSize = sapote_file_size,
    .xLock = sapote_lock,
    .xUnlock = sapote_unlock,
    .xCheckReservedLock = sapote_check_reserved,
    .xFileControl = sapote_file_control,
    .xSectorSize = sapote_sector_size,
    .xDeviceCharacteristics = sapote_device_characteristics
};

static int sapote_open(sqlite3_vfs *vfs, const char *logical,
    sqlite3_file *base, int flags, int *output_flags)
{
    struct sapote_sqlite_file *file = (struct sapote_sqlite_file *)base;
    int open_flags;
    int descriptor;
    int status;

    (void)vfs;
    (void)memset(file, 0, sizeof(*file));
    file->descriptor = -1;
    status = map_path(logical, file->path);
    if (status != SQLITE_OK) return status;
    if ((flags & SQLITE_OPEN_EXCLUSIVE) != 0 && access(file->path, F_OK) == 0) {
        return SQLITE_CANTOPEN;
    }
    open_flags = (flags & SQLITE_OPEN_READWRITE) != 0 ? O_RDWR : O_RDONLY;
    if ((flags & SQLITE_OPEN_CREATE) != 0) open_flags |= O_CREAT;
    descriptor = open(file->path, open_flags);
    if (descriptor < 0 && (flags & SQLITE_OPEN_READWRITE) != 0 &&
        (flags & SQLITE_OPEN_CREATE) == 0) {
        descriptor = open(file->path, O_RDONLY);
        flags = (flags & ~SQLITE_OPEN_READWRITE) | SQLITE_OPEN_READONLY;
    }
    if (descriptor < 0) return SQLITE_CANTOPEN;
    file->descriptor = descriptor;
    file->delete_on_close = (flags & SQLITE_OPEN_DELETEONCLOSE) != 0;
    status = register_file(file);
    if (status != SQLITE_OK) {
        (void)close(descriptor);
        return status;
    }
    file->base.pMethods = &sapote_io;
    if (output_flags != NULL) *output_flags = flags;
    return SQLITE_OK;
}

static int sapote_delete(sqlite3_vfs *vfs, const char *logical, int sync_dir)
{
    char path[FILENAME_MAX];
    (void)vfs;
    if (map_path(logical, path) != SQLITE_OK) return SQLITE_IOERR_DELETE;
    if (unlink(path) != 0 && errno != ENOENT) return SQLITE_IOERR_DELETE;
    if (sync_dir && sapote_syscall1(SAPOTE_SYS_VOLUME_SYNC,
            SAPOTE_VOLUME_DATA) < 0) {
        return SQLITE_IOERR_DIR_FSYNC;
    }
    return SQLITE_OK;
}

static int sapote_access(sqlite3_vfs *vfs, const char *logical, int flags,
    int *result)
{
    char path[FILENAME_MAX];
    int mode = F_OK;
    (void)vfs;
    if (map_path(logical, path) != SQLITE_OK) {
        *result = 0;
        return SQLITE_OK;
    }
    if (flags == SQLITE_ACCESS_READWRITE) mode = R_OK | W_OK;
    else if (flags == SQLITE_ACCESS_READ) mode = R_OK;
    *result = access(path, mode) == 0;
    return SQLITE_OK;
}

static int sapote_full_path(sqlite3_vfs *vfs, const char *logical,
    int capacity, char *output)
{
    char path[FILENAME_MAX];
    const int status = map_path(logical, path);
    const size_t length = status == SQLITE_OK ? strlen(path) : 0U;
    (void)vfs;
    if (status != SQLITE_OK || capacity <= 0 || length >= (size_t)capacity) {
        return SQLITE_CANTOPEN;
    }
    (void)memcpy(output, path, length + 1U);
    return SQLITE_OK;
}

static int sapote_randomness(sqlite3_vfs *vfs, int amount, char *output)
{
    const long result = amount > 0 ? sapote_random(output, (size_t)amount) : 0;
    (void)vfs;
    return result < 0 ? 0 : (int)result;
}

static int sapote_sleep(sqlite3_vfs *vfs, int microseconds)
{
    (void)vfs;
    if (microseconds > 0) (void)usleep((unsigned int)microseconds);
    return microseconds;
}

static int sapote_current_time(sqlite3_vfs *vfs, double *result)
{
    (void)vfs;
    *result = 2440587.5 + (double)sapote_monotonic_ns() /
        (86400.0 * 1000000000.0);
    return SQLITE_OK;
}

static int sapote_last_error(sqlite3_vfs *vfs, int capacity, char *output)
{
    const char *message = strerror(errno);
    size_t length = strlen(message);
    (void)vfs;
    if (capacity <= 0) return errno;
    if (length >= (size_t)capacity) length = (size_t)capacity - 1U;
    (void)memcpy(output, message, length);
    output[length] = '\0';
    return errno;
}

static sqlite3_vfs sapote_vfs = {
    .iVersion = 1,
    .szOsFile = sizeof(struct sapote_sqlite_file),
    .mxPathname = FILENAME_MAX,
    .zName = "sapote",
    .xOpen = sapote_open,
    .xDelete = sapote_delete,
    .xAccess = sapote_access,
    .xFullPathname = sapote_full_path,
    .xRandomness = sapote_randomness,
    .xSleep = sapote_sleep,
    .xCurrentTime = sapote_current_time,
    .xGetLastError = sapote_last_error
};

int sqlite3_os_init(void)
{
    (void)memset(open_files, 0, sizeof(open_files));
    return sqlite3_vfs_register(&sapote_vfs, 1);
}

int sqlite3_os_end(void)
{
    return SQLITE_OK;
}
