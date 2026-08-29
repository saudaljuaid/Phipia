<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Native threads, TLS, and floating-point state

Native processes have at most eight threads, further restricted by the
manifest. Thread creation accepts an executable entry, one integer argument,
an optional FS base, and a 16–64 KiB stack size. The kernel creates a private
RW/NX stack with an unmapped guard and returns a generation-protected thread
handle. Join parks the caller; exit affects the current thread, and the process
ends when no live thread remains.

`TLS_SET` and `TLS_GET` control x86_64 FS base. The C runtime reads the loader's
TLS auxiliary records, allocates and initializes a separate TLS block for each
`pthread`, and selects the local-exec thread pointer. The Rust crate exposes
the raw FS-base contract and raw-entry thread creation; it does not claim Rust
language `#[thread_local]` support.

Futex wait compares one aligned, writable process-local `u32`. A mismatch
returns `-EAGAIN`; a match parks until wake, an absolute deadline, or process
teardown. Wake affects at most the requested number of waiters at the same
address. The SDK builds mutex and once primitives on this path rather than a
userspace spin wait.

Sapote enables the CPU x87/SSE contract before native launch. Each native
thread owns a 16-byte-aligned 512-byte FXSAVE image initialized from `FNINIT`
and `LDMXCSR 0x1f80`. The kernel saves it on native syscalls and interrupts and
restores it before every native thread/process resume. FS base is saved and
cleared on the same boundaries. Kernel C remains compiled with MMX/SSE/SSE2
disabled and soft float, so it does not borrow userspace vector state.

Faulted thread state is never restored into a different thread. Process cleanup
zeros thread records and returns to the kernel address space with FS base zero.

