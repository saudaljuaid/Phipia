<!-- SPDX-License-Identifier: GPL-3.0-only -->

# CPL3 entry, proof return and interrupt frames

Sapote v0.7.0 has one privilege-changing execution boundary. It enters the
validated ELF entry with `IRETQ` and can return only through vector `0x81`, a
temporarily armed DPL3 interrupt gate. This is private proof machinery and not
a native or Linux syscall ABI.

AMD64 APM Volume 2 revision 3.44, Chapter 4 and Sections 8.9 through 8.9.3
define long-mode code/data and 16-byte TSS descriptors, interrupt-gate DPL
checks, TSS privilege-stack selection, the privilege-changing frame and
`IRETQ`. Intel SDM version 092, Volume 3A Chapters 6, 7 and 8 is the cross-check.
The AMD64 psABI 1.0 requires the initial user stack pointer to be 16-byte
aligned.

## Permanent descriptor contract

The existing kernel code/data selectors stay `0x08/0x10`; the 16-byte TSS
descriptor remains at selector `0x18`. Two permanent descriptors are appended:
user data selector `0x2b` and 64-bit user code selector `0x33`. Validation
checks present, DPL3, executable/data type, long-mode and default-size bits and
also checks that TR still names the busy TSS. `cpu_tss_rsp0()` exposes the
validated top of Sapote's bounded RSP0 stack, not an arbitrary caller value.

All 256 IDT entries are normally present DPL0 64-bit interrupt gates. While the
proof token is armed, and only then, vector `0x81` becomes a present DPL3
interrupt gate using the kernel code selector and no IST. Its state is
inactive, armed, entered, returned and disarmed. Vector `0x81` is reserved from
the general handler API.

## Entry frame

`process_enter_user` saves the six System V callee-saved registers and the
kernel continuation stack, then pushes exactly:

```text
user SS = 0x2b
user RSP = 0x0000400000205000
RFLAGS = 0x2
user CS = 0x33
user RIP = 0x0000400000000078
```

All general registers are cleared before `iretq`. The proof runs with maskable
interrupts disabled for its few deterministic instructions; this avoids an
unrelated interrupt and does not change normal interrupt, MSI-X or scheduler
policy. The processor fetches the `mov`, `int` and unreachable `hlt` from the
private user RX mapping.

## Frame ABI

The assembly stubs normalize vector/error code and save GPRs plus CR2. The
common C frame is exactly 168 bytes: CR2 at 0, R15 at 8, RSI at 72, RAX at 120,
vector at 128, error code at 136, RIP at 144, CS at 152 and RFLAGS at 160.

Saved RSP and SS are a separate 16-byte `interrupt_stack_tail` immediately
after that base. Helpers expose it only when saved CS reports a CPL change or
when the vector uses IST. Same-CPL handlers derive interrupted RSP from the
base-frame end and use the kernel data selector; they never read nonexistent
stack-tail words. This preserves inherited exception, PIC, APIC, MSI-X, IST and
preemption frames.

## Authentication and return

The software `int 0x81` gate check admits the DPL3 caller, switches through TSS
RSP0, and saves user SS:RSP. The dispatcher recognizes any CPL3 frame only while
the private gate is armed. It records the exact dispatch-frame address and calls
the registered proof handler; code cannot satisfy the proof by calling that
handler directly.

The handler accepts one delivery with vector `0x81`, error zero, CS/SS
`0x33/0x2b`, RIP at entry plus seven, unchanged stack end, RFLAGS 2, RAX
`0x53415037`, running process identity and active private CR3. A wrong vector,
CPL, selector, RIP, RSP, result, generation, CR3, duplicate or late delivery is
a named proof failure. Controlled user-origin faults during this boundary are
routed to the same failure/teardown continuation; kernel-origin faults retain
the existing fatal policy.

The handler restores kernel CR3 first and authenticates a single aligned kernel
resume stack belonging to the active dispatch. Assembly then abandons the user
interrupt frame, returns on that saved kernel stack, clears the boundary token
and restores the six callee-saved registers exactly. The gate remains owned
until C validates the returned state and disarms it during teardown.
