/* SPDX-License-Identifier: GPL-3.0-only */
#include <pthread.h>

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

#define THREAD_RECORDS 8U
#define DEFAULT_STACK_BYTES (64U * 1024U)

struct thread_record {
    pthread_t handle;
    void *(*entry)(void *);
    void *argument;
    void *result;
    uint64_t tls_address;
    uint64_t tls_length;
    uint64_t thread_pointer;
    volatile uint32_t ready;
    int active;
};

static struct thread_record records[THREAD_RECORDS];
static volatile uint32_t records_lock;
static _Thread_local pthread_t current_thread;

static _Noreturn void thread_trampoline(void *argument)
{
    struct thread_record *record = argument;

    while (__atomic_load_n(&record->ready, __ATOMIC_ACQUIRE) == 0U) {
        const struct sapote_futex_request wait = {
            sizeof(wait), SAPOTE_ABI_VERSION,
            (uint64_t)(uintptr_t)&record->ready, 0U, 0U, 0U
        };
        (void)sapote_syscall1(SAPOTE_SYS_FUTEX_WAIT,
            (uint64_t)(uintptr_t)&wait);
    }
    current_thread = record->handle;
    record->result = record->entry(record->argument);
    (void)sapote_syscall1(SAPOTE_SYS_THREAD_EXIT, 0U);
    __builtin_unreachable();
}

static int prepare_tls(struct thread_record *record)
{
    const struct sapote_startup *startup = sapote_startup_information();
    struct sapote_memory_map_response response;
    uint64_t size;
    uint64_t pointer;

    if (startup->tls_size == 0U) {
        return 0;
    }
    if (startup->tls_alignment == 0U || startup->tls_size > UINT64_MAX -
            startup->tls_alignment) {
        return EINVAL;
    }
    size = startup->tls_size + startup->tls_alignment;
    const long result = sapote_memory_allocate((size_t)size,
        SAPOTE_MEMORY_READ | SAPOTE_MEMORY_WRITE |
            SAPOTE_MEMORY_GUARD_BEFORE | SAPOTE_MEMORY_GUARD_AFTER,
        &response);
    if (result < 0) {
        return (int)-result;
    }
    pointer = (response.address + startup->tls_size +
        startup->tls_alignment - 1U) & ~(startup->tls_alignment - 1U);
    (void)memcpy((void *)(uintptr_t)(pointer - startup->tls_size),
        (const void *)(uintptr_t)startup->tls_image,
        (size_t)startup->tls_size);
    record->tls_address = response.address;
    record->tls_length = response.length;
    record->thread_pointer = pointer;
    return 0;
}

int pthread_create(
    pthread_t *thread,
    const pthread_attr_t *attributes,
    void *(*entry)(void *),
    void *argument
)
{
    struct thread_record *record = NULL;
    struct sapote_thread_create_request request;
    long result;
    int error;

    if (thread == NULL || entry == NULL) return EINVAL;
    sapote_runtime_lock(&records_lock);
    for (size_t index = 0U; index < THREAD_RECORDS; ++index) {
        if (!records[index].active) { record = &records[index]; break; }
    }
    if (record == NULL) { sapote_runtime_unlock(&records_lock); return EAGAIN; }
    (void)memset(record, 0, sizeof(*record));
    record->active = 1;
    record->entry = entry;
    record->argument = argument;
    error = prepare_tls(record);
    if (error != 0) {
        record->active = 0;
        sapote_runtime_unlock(&records_lock);
        return error;
    }
    request.size = sizeof(request);
    request.version = SAPOTE_ABI_VERSION;
    request.entry = (uint64_t)(uintptr_t)thread_trampoline;
    request.argument = (uint64_t)(uintptr_t)record;
    request.tls_base = record->thread_pointer;
    request.stack_bytes = attributes != NULL && attributes->stack_size != 0U ?
        attributes->stack_size : DEFAULT_STACK_BYTES;
    request.flags = 0U;
    result = sapote_syscall1(SAPOTE_SYS_THREAD_CREATE,
        (uint64_t)(uintptr_t)&request);
    if (result < 0) {
        if (record->tls_address != 0U) {
            (void)sapote_memory_release(record->tls_address,
                record->tls_length);
        }
        record->active = 0;
        sapote_runtime_unlock(&records_lock);
        return (int)-result;
    }
    record->handle = (pthread_t)result;
    __atomic_store_n(&record->ready, 1U, __ATOMIC_RELEASE);
    {
        const struct sapote_futex_request wake = {
            sizeof(wake), SAPOTE_ABI_VERSION,
            (uint64_t)(uintptr_t)&record->ready, 0U, 0U, 1U
        };
        (void)sapote_syscall1(SAPOTE_SYS_FUTEX_WAKE,
            (uint64_t)(uintptr_t)&wake);
    }
    *thread = record->handle;
    sapote_runtime_unlock(&records_lock);
    return 0;
}

int pthread_join(pthread_t thread, void **result)
{
    struct thread_record *record = NULL;
    long joined;

    sapote_runtime_lock(&records_lock);
    for (size_t index = 0U; index < THREAD_RECORDS; ++index) {
        if (records[index].active && records[index].handle == thread) {
            record = &records[index];
            break;
        }
    }
    sapote_runtime_unlock(&records_lock);
    if (record == NULL || thread == current_thread) return EINVAL;
    joined = sapote_syscall1(SAPOTE_SYS_THREAD_JOIN, thread);
    if (joined < 0) return (int)-joined;
    if (result != NULL) *result = record->result;
    (void)sapote_handle_close(thread);
    if (record->tls_address != 0U) {
        (void)sapote_memory_release(record->tls_address, record->tls_length);
    }
    sapote_runtime_lock(&records_lock);
    (void)memset(record, 0, sizeof(*record));
    sapote_runtime_unlock(&records_lock);
    return 0;
}

_Noreturn void pthread_exit(void *result)
{
    for (size_t index = 0U; index < THREAD_RECORDS; ++index) {
        if (records[index].active && records[index].handle == current_thread) {
            records[index].result = result;
            break;
        }
    }
    (void)sapote_syscall1(SAPOTE_SYS_THREAD_EXIT, 0U);
    __builtin_unreachable();
}

pthread_t pthread_self(void) { return current_thread; }
int pthread_equal(pthread_t left, pthread_t right) { return left == right; }
int pthread_mutex_init(pthread_mutex_t *mutex, const void *attributes)
{ (void)attributes; if (mutex == NULL) return EINVAL; mutex->value = 0U; return 0; }
int pthread_mutex_destroy(pthread_mutex_t *mutex)
{ if (mutex == NULL || mutex->value != 0U) return EBUSY; return 0; }
int pthread_mutex_lock(pthread_mutex_t *mutex)
{ if (mutex == NULL) return EINVAL; sapote_runtime_lock(&mutex->value); return 0; }
int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
    uint32_t expected = 0U;
    if (mutex == NULL) return EINVAL;
    return __atomic_compare_exchange_n(&mutex->value, &expected, 1U, 0,
        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ? 0 : EBUSY;
}
int pthread_mutex_unlock(pthread_mutex_t *mutex)
{ if (mutex == NULL || mutex->value == 0U) return EINVAL; sapote_runtime_unlock(&mutex->value); return 0; }
int pthread_once(pthread_once_t *once, void (*function)(void))
{
    uint32_t expected = 0U;
    if (once == NULL || function == NULL) return EINVAL;
    if (__atomic_compare_exchange_n(&once->state, &expected, 1U, 0,
            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        function();
        __atomic_store_n(&once->state, 2U, __ATOMIC_RELEASE);
        const struct sapote_futex_request wake = {sizeof(wake), SAPOTE_ABI_VERSION,
            (uint64_t)(uintptr_t)&once->state, 0U, 0U, UINT32_MAX};
        (void)sapote_syscall1(SAPOTE_SYS_FUTEX_WAKE, (uint64_t)(uintptr_t)&wake);
    } else while (__atomic_load_n(&once->state, __ATOMIC_ACQUIRE) != 2U) {
        const struct sapote_futex_request wait = {sizeof(wait), SAPOTE_ABI_VERSION,
            (uint64_t)(uintptr_t)&once->state, 0U, 1U, 0U};
        (void)sapote_syscall1(SAPOTE_SYS_FUTEX_WAIT, (uint64_t)(uintptr_t)&wait);
    }
    return 0;
}
int pthread_attr_init(pthread_attr_t *attributes)
{ if (attributes == NULL) return EINVAL; attributes->stack_size = DEFAULT_STACK_BYTES; return 0; }
int pthread_attr_destroy(pthread_attr_t *attributes)
{ return attributes == NULL ? EINVAL : 0; }
int pthread_attr_setstacksize(pthread_attr_t *attributes, uint32_t size)
{
    if (attributes == NULL || size < 16384U || size > 65536U) return EINVAL;
    attributes->stack_size = size; return 0;
}
