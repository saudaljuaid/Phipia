/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_RUNTIME_H
#define SAPOTE_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include <sapote/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sapote_startup {
    int argc;
    char **argv;
    char **environment;
    uint64_t tls_image;
    uint64_t tls_size;
    uint64_t tls_alignment;
};

long sapote_syscall0(uint64_t number);
long sapote_syscall1(uint64_t number, uint64_t argument0);
long sapote_syscall2(uint64_t number, uint64_t argument0, uint64_t argument1);
long sapote_syscall3(uint64_t number, uint64_t argument0, uint64_t argument1,
    uint64_t argument2);
long sapote_syscall4(uint64_t number, uint64_t argument0, uint64_t argument1,
    uint64_t argument2, uint64_t argument3);
long sapote_syscall5(uint64_t number, uint64_t argument0, uint64_t argument1,
    uint64_t argument2, uint64_t argument3, uint64_t argument4);
long sapote_syscall6(uint64_t number, uint64_t argument0, uint64_t argument1,
    uint64_t argument2, uint64_t argument3, uint64_t argument4,
    uint64_t argument5);

void sapote_runtime_initialize(int argc, char **argv, char **environment);
const struct sapote_startup *sapote_startup_information(void);
int sapote_result(long result);
long sapote_handle_close(sapote_handle_t handle);
long sapote_memory_allocate(size_t length, uint32_t flags,
    struct sapote_memory_map_response *response);
long sapote_memory_release(uint64_t address, uint64_t length);
long sapote_file_open(uint16_t volume, const char *path, uint32_t flags);
long sapote_file_read(sapote_handle_t handle, void *buffer, size_t length);
long sapote_file_write(sapote_handle_t handle, const void *buffer,
    size_t length);
long sapote_file_seek(sapote_handle_t handle, int64_t offset,
    uint32_t origin);
long sapote_path_stat(uint16_t volume, const char *path,
    struct sapote_path_stat *result);
long sapote_directory_open(uint16_t volume, const char *path);
long sapote_directory_read(sapote_handle_t handle,
    struct sapote_directory_entry *entry);
uint64_t sapote_monotonic_ns(void);
long sapote_sleep_until(uint64_t deadline_ns);
long sapote_random(void *buffer, size_t length);

#ifdef __cplusplus
}
#endif

#endif
