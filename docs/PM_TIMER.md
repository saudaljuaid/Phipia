# ACPI power management timer

Sapote had two clocks before this increment, and both were calibrated against
the PIT. They agreed with each other, and that agreement proved less than it
looked like it did.

## Why an unmeasured reference is the point

Two clocks derived from one ruler catch a mistake in either clock. They cannot
catch a mistake in the ruler. If the PIT measurement is wrong, both derived
rates are wrong by the same factor, every interval computed from them is wrong
by that factor, and the two clocks still agree perfectly. `docs/TSC.md` recorded
that limitation. This increment closes it.

The ACPI power management timer is different in kind. ACPI 6.6 section 4.8.3.3
fixes its frequency at exactly 3.579545 MHz, a third of the colour burst
frequency the original PC clock chain was built from. Sapote does not measure
that rate against anything, so the timer is not a third opinion drawn from the
same evidence — it is the first opinion here that owes the PIT nothing.

## What it found on its first run

It disagreed. The new timer said an interval was half as long as the two
calibrated clocks said it was, by a factor of 2.00 in both cases:

```text
Sapote: PM timer measured 9999874 ns against TSC 20172796 ns
Sapote: local APIC timer calibrated at 31299710 counts per second
```

The PIT was programmed in mode 3, the square wave generator. Mode 3 toggles its
output twice per programmed period, once at the half count and once at the full
count, so an edge-triggered handler counts **two** interrupts for every period
the divisor describes. `pit_wait_for_ticks(10)` at 100 Hz therefore returned
after 50 ms rather than 100 ms, and every rate calibrated against it came out at
half its true value.

Two independent measurements confirm the diagnosis and agree with each other to
0.03%:

- the PM timer, at its specified rate, measured 100 PIT ticks at a programmed
  100 Hz as spanning 1790310 ticks — 0.5002 seconds, so 200 Hz;
- QEMU's local APIC timer counts one tick per 16 ns under divide-by-sixteen, so
  62.5 MHz exactly. Sapote measured 31.3 MHz — half.

The fix is one control word: `src/kernel/pit.c` now programs mode 2, the rate
generator, which drives its output low for one input clock once per period.
After it, the same three measurements agree:

```text
Sapote: local APIC timer calibrated at 62643780 counts per second
Sapote: TSC calibrated at 2806852220 Hz, invariant no
Sapote: PM timer measured 9999874 ns against TSC 10130453 ns
```

62.64 MHz against the 62.5 MHz QEMU models is a 0.2% match, derived without
reference to the PM timer at all. The correction is included here because
without it the agreement this increment exists to demonstrate does not exist,
and reporting agreement at a tolerance loose enough to hide a factor of two
would be worse than reporting nothing.

## Finding the timer

The timer's port lives in the FADT, signature `FACP`, one of the tables the
RSDT or XSDT references. `acpi_fadt_discover` reuses the walk
`acpi_madt_discover` uses: `find_unique_table` validates the root, then bounds
checks and checksums every referenced table before any reader sees it, and
refuses a firmware that declares the same table twice rather than resolving the
conflict by position.

ACPI 6.6 section 5.2.9 places the fields at fixed byte offsets from the start of
the table:

| Field | Offset | Size | Meaning |
| --- | --- | --- | --- |
| `PM_TMR_BLK` | 76 | 4 | I/O port of the counter |
| `PM_TMR_LEN` | 91 | 1 | must be 4, or the timer is absent |
| `FLAGS` | 112 | 4 | bit 8 `TMR_VAL_EXT`: 0 = 24-bit counter, 1 = 32-bit |
| `X_PM_TMR_BLK` | 208 | 12 | ACPI 2.0+ generic address structure |

An ACPI 1.0 table ends at 116 bytes, immediately past `FLAGS`. Reading
`X_PM_TMR_BLK` out of one would be reading memory the table does not describe,
so it is read only when the table's own declared length reaches offset 220. The
three fixed fields are proved to fit inside the ACPI 1.0 span by
`_Static_assert` rather than by a per-field bounds branch no test could reach.

The supported QEMU target publishes a revision 1 FADT, so it exercises the
fixed-field path:

```text
Sapote: ACPI FADT at 0x0000000007FE1B06 revision 1 flags 0x00000000000080A5
Sapote: ACPI PM timer port 0x0000000000000608 width 24 bits address fixed
```

### Which address wins

ACPI 6.6 gives `X_PM_TMR_BLK` precedence: when it holds an address the operating
system can use, `PM_TMR_BLK` must be ignored. Sapote applies that literally.

The extended block is *selected* when the declared length covers it, its address
is non-zero, and its address space is System I/O. A block in any other space
describes a register no port read can reach, so it selects nothing and the fixed
field stands — that is a fallback, not a guess, and the boot line reports which
one was used.

Once selected, the block must be well formed. A bit width other than 32 or a
non-zero bit offset contradicts the register the specification describes, and a
contradiction is refused with `ACPI_STATUS_BAD_PM_TIMER_BLOCK` rather than
resolved by preferring whichever field looks more plausible.

### Refusals

Firmware chooses every length, port and flag on this path, so each is checked
before it is believed, and every rejection zeroes the output structure so a
partial result can never be mistaken for a complete one:

- no FADT in the root, or two of them;
- a table shorter than the fields ACPI 1.0 guarantees;
- `PM_TMR_LEN` that is not 4 — zero is how firmware says the timer is absent,
  and anything else describes a register this reader cannot address;
- neither address present;
- a selected extended block contradicting the register's width or offset;
- a port whose four-byte block would run past the end of the I/O address space.

## Reading the counter

A 32-bit port read, which `include/sapote/cpu.h` did not previously have.
`cpu_in32` is a four-instruction `inl` in `src/arch/x86_64/cpu.S`; the timer
block is a single dword register and the specification gives no meaning to
reading it in narrower pieces.

A 24-bit counter leaves the register's upper bits reserved, so `pm_timer_read`
masks the sample to the decoded width and no caller ever sees a bit the counter
does not own.

## The wrap

The counter wraps silently, at 2^24 or 2^32 depending on `TMR_VAL_EXT`. Unsigned
subtraction is already modular, so masking the difference to the counter's own
width folds exactly one wrap out without a comparison: an end that reads below
the start is a wrap, not an error.

The width is a decoded property carried alongside the port, never a hardcoded
32. Folding a 24-bit counter at 32 bits would turn every wrapped span into a
number roughly 255 times too large.

Two wraps are indistinguishable from one, so a zero span is refused rather than
reported: either the counter did not advance, or it advanced exactly one full
period, and neither is a duration. That ambiguity is also why `pm_timer_wait`
accumulates the difference between *consecutive* samples instead of subtracting
its first sample from its last — consecutive samples are one port read apart,
far inside a single period, so a total may run past a period without ever
becoming ambiguous.

## Bounded waiting

`CONTRIBUTING.md` forbids unbounded waits. A wait may request at most a quarter
of the narrowest counter's period, about 1.17 seconds, and the spin allows
sixty-four polls for every requested tick. Each poll is one port read, which
cannot be cheaper than a bus transaction, so a counter running at 3.579545 MHz
advances well within that budget; a counter that has stopped answering returns
`PM_TIMER_STATUS_STALLED` instead of hanging the kernel.

## Executable proof

`pm_timer_self_test` proves the arithmetic without hardware, because a mistake
there is silent: a mis-folded wrap comes out as a plausible duration rather than
as an error. The port read is deliberately kept outside `wait_begin` and
`wait_step` so the wrap folding, the completion test and the poll bound are all
reachable from synthetic samples. It covers a span at each width, a 24-bit wrap,
a 32-bit wrap, a 24-bit sample carrying reserved upper bits, a difference that
exists only above bit 23, a zero span, an undefined counter width, a wait
satisfied by its first sample, a wait that wraps midway through, a stuck counter
exhausting the poll bound, both interval bounds and the largest wait still
allowed, both conversions in both directions, a sub-second remainder that must
not be divided away, an hour of ticks converting without overflow, the agreement
policy at and just past its boundary, and every initialization refusal — each of
which must leave the subsystem absent.

`acpi_tables_self_test` proves the FADT decode against a synthetic fixture: the
extended address winning when well formed, `TMR_VAL_EXT` widening the counter
and nothing else, the fixed field taking over when the extended address is
absent or in an unreachable address space, an ACPI 1.0 table never reading past
its own length, and every refusal above.

The `pm-timer` QEMU scenario proves the hardware path and the agreement. It
checks the timer was discovered, refuses a second description, requires the
counter to advance on its own within its bound, then calibrates both PIT-derived
clocks and has the local APIC timer define a 200 ms interval by counting twenty
of its own ticks at 100 Hz. The PM timer and the TSC each measure that same
interval without being told what it should be:

```text
ST INFO pm-timer: PM 200543644 ns, APIC timer 200000000 ns, TSC 200761000 ns
```

### The two tolerances

The local APIC timer comparison is held to a quarter of the reference. Across
ten consecutive runs the observed disagreement was +0.22% to +0.42%, so the
bound carries roughly a sixtyfold margin while still failing a rate wrong by a
factor of two — a 50% disagreement — with twice the margin needed. This is the
comparison that guards against the class of error described above.

The TSC comparison is held to a half. Under emulation the PIT and the local APIC
timer are both driven from the host's monotonic clock, so a scheduling stall
delays them together and their ratio survives it. The TSC is derived from the
host's own counter, a different clock, so a stall landing inside its 100 ms
calibration window is absorbed into the rate it calibrates to and skews every
duration afterwards. That was observed once in fifteen runs, at 23%, while the
PM timer and the APIC timer agreed to 0.29% in the same run. The `tsc` scenario
already allows a factor of two for this reason.

A half fails only errors beyond a factor of two, which is deliberate and costs
nothing here: the factor-of-two case is caught by the APIC timer comparison,
which does not cross clock domains.

Normal boot additionally requires:

```text
Sapote: ACPI FADT verified
Sapote: ACPI PM timer port 0x0000000000000608 width 24 bits address fixed
Sapote: PM timer measured 9999874 ns against TSC 10130453 ns
Sapote: PM timer independent reference established
```

## Negative controls

A passing test that cannot fail proves nothing. Three deliberate breakages,
each isolating a different layer, were run and then reverted:

| Breakage | Observed failure |
| --- | --- |
| `PM_TIMER_FREQUENCY_HZ` multiplied by four | `Sapote PANIC: ACPI PM timer arithmetic self-test failed` |
| `PIT_CHANNEL_ZERO_MODE_RATE_GENERATOR` reverted to mode 3 | `ST FAIL pm-timer: PM timer and local APIC timer disagree on interval`, reporting `PM 100250171 ns, APIC timer 200000000 ns` |
| decoded port advanced by four past validation | `ST FAIL pm-timer: ACPI PM timer did not advance within its bound` |

The second is the one that matters most: it is the regression this increment
was built to detect, and the scenario detects it.

## Deferred work

The PIT still existed when this was written, and the APIC timer and the TSC were
still calibrated against it. `docs/PIT_RETIREMENT.md` covers the increment that
followed: both clocks recalibrated against this timer, and the 8254 stopped,
masked and latched shut. It was founded on the evidence this increment produced
rather than on assumption — three clocks agreeing about one interval, one of which
was never told how long the interval was.

This timer is not yet a general time base. It is read by polling, so nothing here
delivers an interrupt or maintains a running clock, and a duration longer than a
quarter of the counter's period is refused rather than accumulated. A monotonic
clock and deadline-based timers are what turn these rates into something a
scheduler can use, and neither exists yet.

~~Level-triggered I/O APIC routing still needs directed EOI.~~ **Fixed**, in
`docs/IO_APIC.md`. Nothing here is
per-processor; the ACPI timer is a single platform-wide counter, which is an
advantage a second processor will want, but no part of this kernel is multi-core
yet.
