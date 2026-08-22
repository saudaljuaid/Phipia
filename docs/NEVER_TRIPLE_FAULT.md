# Never Triple Fault

This milestone gives Sapote a deterministic failure boundary. Every x86_64 IDT
vector is present, every entry reaches a normalized Assembly ABI, and every
unexpected event either returns through a registered handler or emits a bounded
diagnostic and halts. A dedicated double-fault path does not call ordinary C.

## Interrupt-entry ABI

AMD64 long mode always pushes `RFLAGS`, `CS`, and `RIP` as eight-byte values.
It additionally pushes the old `SS` and `RSP` for a privilege change or IST
switch. Vectors 8, 10-14, 17, 21, 29, and 30 also receive a hardware error
code. Their stubs push only the vector; every other stub pushes a zero error
code followed by the vector.

The common entry clears the direction flag, saves all fifteen non-RSP general
registers, snapshots CR2, aligns the live stack to the System V call boundary,
and calls ordinary freestanding C. The original frame pointer is held in a
callee-saved register. Return restores the exact saved stack, registers, vector
and error slots before executing `iretq`.

The common C structure is statically asserted to 168 bytes. A separate 16-byte
stack tail describes saved RSP/SS only when saved CS or the IST route proves it
exists; same-CPL code never reads beyond the common frame. Changing either
structure, the Assembly push order, the list of error-code vectors or tail
classification is one ABI change and must be reviewed as such.

## Descriptor-table invariants

The permanent GDT contains a null descriptor, ring-zero code and data, DPL3
data and 64-bit code, and one 64-bit TSS descriptor. The TSS owns independent
16 KiB stacks for RSP0, double
fault, NMI, and machine check. IST1, IST2, and IST3 select the latter three.
Vector `0xF0` temporarily shares IST1 solely for a recoverable stack-routing
proof. Vector `0x81` is normally DPL0 and becomes one private DPL3 interrupt
gate only while the v0.7.0 CPL3 proof token is armed; see
`docs/CPL3_INTERRUPT_BOUNDARY.md`.

The IDT is the first subsystem initialized by C. It is first loaded without IST
selectors while the bootstrap GDT is still active. Sapote then loads the
permanent GDT and task register, patches the live IDT with IST selectors, and
validates GDTR, IDTR, TR, all 256 gates, the TSS, and stack canaries before the
boot parser, memory allocator, or any `sti`.

Canaries detect some downward overflow but are not guard pages. The early 2 MiB
identity map cannot provide real guard pages; that belongs to the virtual-memory
milestone.

## Interrupt-controller invariants

Both 8259 PICs are masked before descriptor work. Initialization remaps them to
vectors `0x20-0x2F` and leaves every IRQ masked. The PIT registers its handler
and completes its state before IRQ0 is unmasked. IRQ handlers run with nesting
disabled and the dispatcher sends the EOI after the handler returns.

Spurious IRQ7 receives no EOI. Spurious IRQ15 receives an EOI only on the master
PIC. Genuine slave interrupts receive slave then master EOIs.

## Double-fault containment

Vector 8 enters a fixed Assembly-only routine on IST1. It performs bounded COM1
polling, writes a fixed VGA diagnostic, and halts. It deliberately avoids the C
dispatcher, allocator, mutable console cursor, formatting, and locks. A failure
inside an elaborate double-fault report would become the triple fault this
milestone exists to prevent.

## Executable proof

`make qemu-tests` boots one kernel under eight Multiboot command-line scenarios:

1. normal descriptor and frame validation;
2. breakpoint return with every GPR and the direction flag restored;
3. fatal invalid opcode with an exact fault RIP;
4. non-present page fault at 4 GiB with exact CR2 and decoded error bits;
5. recoverable entry through the double-fault IST stack;
6. synthetic spurious IRQ7/IRQ15 paths and eight PIT interrupts, which also
   prove repeated genuine EOIs;
7. deterministic handling of an unregistered vector;
8. a genuine double fault created by making page-fault delivery fail.

Each guest prints exactly one `ST BEGIN <scenario>` and one matching
`ST PASS <scenario>`, then writes a scenario-specific value to QEMU's test-only
exit device. The host requires the exact transformed status. Timeouts, ordinary
exit, duplicate markers, stale logs, panic text, and missing markers all fail.
Consequently, a reset, hang, malformed `iretq`, double fault, or triple fault
cannot be mistaken for success.

## Deferred work

This remains single-core and has no `swapgs`, nested interrupts, SMP state or
interrupt-safe console lock. The sole userspace frame is the bounded v0.7.0
proof; it is not a general fault-containment or syscall model. General
per-process interrupt policy must arrive with its own executable proofs.
