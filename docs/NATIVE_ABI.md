<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Native userspace ABI v1

The Sapote native ABI is the kernel contract for static Ring 3 applications.
It is separate from the three measured Linux/BusyBox profiles. The canonical
machine-readable definitions are under `include/sapote/abi/`; the SDK installs
the same headers under `sdk/include/sapote/abi/`.

## Calling and result convention

On x86_64, `RAX` contains the syscall number. Arguments zero through five use
`RDI`, `RSI`, `RDX`, `R10`, `R8`, and `R9`. `SYSCALL` destroys `RCX` and `R11`.
Results are returned in `RAX`: zero or a positive value is success and a
negative `sapote_errno` value is failure. Unknown numbers return `-ENOSYS`.

Every public record uses fixed-width fields, begins with `size` and `version`
where evolution is expected, and names its reserved fields. Callers set every
reserved field and unused flag bit to zero. Version 1 rejects unknown flags;
it never silently accepts a future meaning. ABI records are packed and have
compile-time size assertions, so their layout does not depend on compiler
padding.

The kernel copies user data through checked aliases. It validates the entire
range with checked arithmetic before a promised all-or-nothing copy. A range
is rejected if it wraps, is noncanonical, crosses an unmapped page, has the
wrong direction permission, or names a guard, kernel, MMIO, DMA, page-table,
foreign-process, immutable executable, or read-only output mapping.

## Service groups

| Range | Calls | Blocking and ownership |
| --- | --- | --- |
| `0x0000` | ABI version, exit, console read/write, handle close/duplicate | Writes are immediate. Console read parks the thread until keyboard input. Close consumes one handle immediately; duplicate creates another reference to the same typed object. |
| `0x0100` | anonymous map and unmap | Synchronous. Maps are private, page-granular, RW/NX or R/NX, optionally guarded, and charged to the manifest limit. Successful unmap consumes the complete named mapping. |
| `0x0200` | file, directory, path mutation, stat, seek, sync, free space | Synchronous bounded FAT32 operations. Open calls return owned typed handles. Reads and writes may return a documented partial byte count; metadata mutations either publish a valid result or fail. |
| `0x0300` | monotonic time, sleep, wait, entropy, timers, cancellation | Sleep and wait park only the calling native thread. Wait copies at most eight items into the kernel and copies the full set back on completion. Timeout is `-ETIMEDOUT`; cancellation is `-ECANCELED`. |
| `0x0400` | window creation, surface present, event read, pointer capture | Window creation returns owned window and event-queue handles plus one process-local RW/NX surface mapping. Present consumes no ownership. Event read is nonblocking; wait on the queue before retrying. |
| `0x0500` | DNS, TCP, UDP, address query | Open calls return owned stream/datagram handles. Deadlines are absolute monotonic nanoseconds. Shutdown changes stream direction state but does not close the handle. |
| `0x0600` | thread create/exit/join, FS-base TLS, futex wait/wake | Create returns an owned thread handle. Join parks and reports the target exit status; the handle is still closed explicitly. Futex wait compares one aligned user `u32` before parking. |

Directory enumeration reports the canonical printable form of each accepted
ASCII 8.3 name in lower case. Path lookup remains case-insensitive.

Network readiness waits park a native thread in the scheduler. Synchronous
DNS and stream/datagram operations pump bounded protocol state, recheck their
absolute deadline and completion state, then halt the core until a device or
timer interrupt. They never poll in a userspace or kernel spin loop.

`include/sapote/abi/base.h` is the syscall-number and error-number registry.
The service-specific headers define exact records, limits, flags, event values,
pixel format, IPv4 endpoint encoding, and static size checks.

## Concurrency and cleanup

Handles and mappings are process-local. Version 1 has no implicit inheritance
and no cross-process transfer. Syscalls may be issued by any live thread;
kernel handle resolution and resource mutation occur with the native scheduler
in kernel context. Wait, sleep, join, console read, and futex wait save the
thread context and let another runnable thread or process proceed.

Normal exit, the final thread exiting, a userspace exception, or a failed
admission path converges on process teardown. Teardown closes every handle,
destroys windows and queues, cancels network ownership, releases every private
frame and surface mapping, clears saved FPU/TLS state, and restores the kernel
address space before the result is published.
