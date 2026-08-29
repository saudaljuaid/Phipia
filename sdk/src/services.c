/* SPDX-License-Identifier: GPL-3.0-only */
#include <sapote/event.h>
#include <sapote/network.h>
#include <sapote/runtime.h>
#include <sapote/window.h>

#include <errno.h>
#include <string.h>

long sapote_wait(struct sapote_wait_item *items, size_t count,
    uint64_t deadline_ns)
{
    const struct sapote_wait_request request = {sizeof(request),
        SAPOTE_ABI_VERSION, (uint64_t)(uintptr_t)items, deadline_ns,
        (uint32_t)count, 0U};

    if (items == NULL || count == 0U || count > SAPOTE_WAIT_MAX) {
        return -SAPOTE_EINVAL;
    }
    return sapote_syscall1(SAPOTE_SYS_WAIT,
        (uint64_t)(uintptr_t)&request);
}

long sapote_timer_create(void)
{
    return sapote_syscall0(SAPOTE_SYS_TIMER_CREATE);
}

long sapote_timer_set(sapote_handle_t timer, uint64_t deadline_ns)
{
    const struct sapote_timer_set_request request = {sizeof(request),
        SAPOTE_ABI_VERSION, timer, deadline_ns, 0U, 0U};

    return sapote_syscall1(SAPOTE_SYS_TIMER_SET,
        (uint64_t)(uintptr_t)&request);
}

long sapote_cancel(sapote_handle_t handle)
{
    return sapote_syscall1(SAPOTE_SYS_CANCEL, handle);
}

int sapote_window_create(const char *title, uint32_t width, uint32_t height,
    struct sapote_window_create_response *response)
{
    const struct sapote_window_create_request request = {
        sizeof(request), SAPOTE_ABI_VERSION, (uint64_t)(uintptr_t)title,
        (uint32_t)strlen(title), width, height, SAPOTE_PIXEL_XRGB8888, 0U, 0U
    };
    return sapote_result(sapote_syscall2(SAPOTE_SYS_WINDOW_CREATE,
        (uint64_t)(uintptr_t)&request, (uint64_t)(uintptr_t)response));
}
long sapote_surface_present(sapote_handle_t window,
    const struct sapote_rect *rectangles, size_t count)
{
    const struct sapote_present_request request = {sizeof(request),
        SAPOTE_ABI_VERSION, window, (uint64_t)(uintptr_t)rectangles,
        (uint32_t)count, 0U};
    return sapote_syscall1(SAPOTE_SYS_SURFACE_PRESENT,
        (uint64_t)(uintptr_t)&request);
}
long sapote_event_read(sapote_handle_t events, struct sapote_event *event)
{ return sapote_syscall2(SAPOTE_SYS_EVENT_READ, events, (uint64_t)(uintptr_t)event); }
long sapote_event_wait(sapote_handle_t events, uint64_t deadline_ns)
{
    struct sapote_wait_item item = {events, SAPOTE_WAIT_READABLE, 0U};
    const struct sapote_wait_request request = {sizeof(request),
        SAPOTE_ABI_VERSION, (uint64_t)(uintptr_t)&item, deadline_ns, 1U, 0U};
    return sapote_syscall1(SAPOTE_SYS_WAIT, (uint64_t)(uintptr_t)&request);
}
long sapote_pointer_capture(sapote_handle_t window, int capture)
{ return sapote_syscall2(SAPOTE_SYS_POINTER_CAPTURE, window, capture != 0); }

long sapote_dns_resolve(const char *hostname, uint64_t deadline_ns)
{ return sapote_syscall3(SAPOTE_SYS_DNS_RESOLVE, (uint64_t)(uintptr_t)hostname, strlen(hostname), deadline_ns); }
long sapote_stream_open(void) { return sapote_syscall0(SAPOTE_SYS_STREAM_OPEN); }
long sapote_stream_connect(sapote_handle_t stream,
    const struct sapote_ipv4_endpoint *endpoint, uint64_t deadline_ns)
{ return sapote_syscall3(SAPOTE_SYS_STREAM_CONNECT, stream, (uint64_t)(uintptr_t)endpoint, deadline_ns); }
static long network_io(uint64_t number, sapote_handle_t handle, void *buffer,
    size_t length, uint64_t deadline, struct sapote_ipv4_endpoint *endpoint)
{
    struct sapote_network_io request = {sizeof(request), SAPOTE_ABI_VERSION,
        handle, (uint64_t)(uintptr_t)buffer, deadline, {0U, 0U, 0U},
        (uint32_t)length, 0U};
    if (endpoint != NULL) request.endpoint = *endpoint;
    const long result = sapote_syscall1(number, (uint64_t)(uintptr_t)&request);
    if (result >= 0 && endpoint != NULL &&
        number == SAPOTE_SYS_DATAGRAM_RECEIVE) *endpoint = request.endpoint;
    return result;
}
long sapote_stream_read(sapote_handle_t stream, void *buffer, size_t length,
    uint64_t deadline_ns)
{ return network_io(SAPOTE_SYS_STREAM_READ, stream, buffer, length, deadline_ns, NULL); }
long sapote_stream_write(sapote_handle_t stream, const void *buffer,
    size_t length, uint64_t deadline_ns)
{ return network_io(SAPOTE_SYS_STREAM_WRITE, stream, (void *)(uintptr_t)buffer, length, deadline_ns, NULL); }
long sapote_stream_shutdown(sapote_handle_t stream, uint32_t flags,
    uint64_t deadline_ns)
{ return sapote_syscall3(SAPOTE_SYS_STREAM_SHUTDOWN, stream, flags, deadline_ns); }
long sapote_datagram_open(void) { return sapote_syscall0(SAPOTE_SYS_DATAGRAM_OPEN); }
long sapote_datagram_bind(sapote_handle_t datagram, uint16_t port)
{ return sapote_syscall2(SAPOTE_SYS_DATAGRAM_BIND, datagram, port); }
long sapote_datagram_send(sapote_handle_t datagram,
    const struct sapote_ipv4_endpoint *destination, const void *buffer,
    size_t length, uint64_t deadline_ns)
{ struct sapote_ipv4_endpoint endpoint = *destination; return network_io(SAPOTE_SYS_DATAGRAM_SEND, datagram, (void *)(uintptr_t)buffer, length, deadline_ns, &endpoint); }
long sapote_datagram_receive(sapote_handle_t datagram,
    struct sapote_ipv4_endpoint *source, void *buffer, size_t length,
    uint64_t deadline_ns)
{ return network_io(SAPOTE_SYS_DATAGRAM_RECEIVE, datagram, buffer, length, deadline_ns, source); }
long sapote_network_address(sapote_handle_t handle, int peer,
    struct sapote_ipv4_endpoint *endpoint)
{ return sapote_syscall3(SAPOTE_SYS_NETWORK_ADDRESS, handle, peer != 0, (uint64_t)(uintptr_t)endpoint); }
long sapote_network_cancel(sapote_handle_t handle)
{ return sapote_syscall1(SAPOTE_SYS_CANCEL, handle); }
