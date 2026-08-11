# Local APIC timer and monotonic timebase

This milestone extends Zenith's active xAPIC subsystem with one calibrated,
periodic Local APIC timer on the bootstrap processor. The PIT is retained only
as a bounded calibration reference. After calibration, its I/O APIC
redirection entry is masked and vector `0x30` is the only active kernel timer
vector. The implementation remains single-core. Vector `0x30` requests a
scheduler decision every four ticks; switching occurs after its EOI at the
outermost eligible interrupt exit.

## Timer invariants

The Local APIC timer is accepted only after the existing APIC capability,
identity, mapping, version, and LVT-capacity checks pass. Divide configuration
`0x03` selects divide-by-16. Zenith rejects reserved divide bits and proves all
eight architectural encodings in a pure self-test. The LVT vector, mask, and
mode fields, divide register, initial count, and current-count boundary are
read back before the timer is published.

The APIC-routed PIT remains on vector `0x20`; the Local APIC timer owns the
dedicated vector `0x30`. Both use the interrupt dispatcher's Local APIC EOI
path. The spurious vector remains `0xFF` and receives no EOI. The Local APIC
timer is programmed in periodic mode only after its handler exists and the PIT
route has been masked and verified. All controller mutation runs with
`RFLAGS.IF=0`.

Stopping follows the inverse publication order: mask and zero the timer,
verify that stopped hardware state, unregister its handler, and only then clear
the active configuration. A failed readback or handler operation retains the
software configuration so the bounded stop can be inspected and retried. The
PIT route-mask cache likewise changes only after matching I/O APIC readback.
If timebase validation then needs rollback and stop fails, the time subsystem
keeps a distinct rollback-pending state and rejects clock operations with a
dedicated status. Calling initialization again with `IF=0` first retries the
same stop; only successful cleanup clears the retained state and begins a fresh
bounded calibration.

The active target is 1,000 interrupts per second. The calibrated divided-clock
frequency must be at least 100 kHz, the periodic target must be within 10 Hz to
10 kHz, the derived initial count must be at least 100, and its rounded output
frequency must be within one percent of the requested rate. These are Zenith
policy bounds chosen to reject stopped timers, implausible arithmetic, and
excessive quantization before activation.

## Calibration procedure

The PIT uses mode 3 at a requested 20 Hz and its architectural 1,193,182 Hz
input. The lower interrupt rate and longer sample window keep the reference
observable under emulation load. Its programmed 16-bit divisor, rather than a
truncated nominal frequency, is used in all frequency arithmetic.

Zenith collects exactly five accepted samples. Each attempt first synchronizes
to a PIT delivery, programs the masked Local APIC timer as a one-shot down-counter from
`UINT32_MAX` at divide-by-16, and observes 32 further PIT deliveries. Thus, a
nominal sample spans 1.6 seconds rather than depending on a short high-rate
burst. The actual PIT tick delta is retained so additional deliveries cannot
bias the result. A sample is rejected if either counter fails to advance, the
Local APIC counter expires, the PIT delta exceeds 64, a register readback
differs, or the derived frequency is outside the 32-bit policy range.

Counter non-advance, expiry, a PIT delta above 64, or an out-of-policy derived
frequency discards that attempt instead of publishing suspect data. Candidates
are kept in frequency order; as soon as a five-sample window satisfies the
existing 5% median-spread rule, exactly that coherent window is accepted.
Collection remains bounded to seven total attempts. Register, PIT, or wait
failures still abort immediately, and exhausting the two spare attempts without
a coherent window returns the existing bad-sample status. No per-wait ceiling,
frequency tolerance, or scenario timeout is increased. This absorbs up to two
short emulator deschedules while still failing deterministically under
persistent timing distortion.

Each sample frequency is calculated with checked 64-bit integer arithmetic:

```text
round(local_apic_counts * 1193182 / (pit_divisor * pit_ticks))
```

The five frequencies are sorted, their median becomes the calibration result,
and every sample must be within five percent of that median. No floating point
is used. The periodic initial count and nanoseconds-per-tick value use rounded
64-bit division whose largest intermediate is below `UINT64_MAX`.

Every PIT and Local APIC wait has a fixed 100,000,000-iteration ceiling. The
CPU enables interrupts while polling a volatile counter and uses `pause`; it
does not enter an unbounded `hlt`. A missing or stopped reference therefore
returns a timeout with interrupts disabled.

## Monotonic-time contract

`include/zenith/time.h` exposes ticks, the active frequency in hertz, the
period in nanoseconds, checked tick-to-nanosecond conversion, a nanosecond
snapshot, validation, and a bounded test wait. Conversion first proves that
`ticks * period_nanoseconds` fits in `uint64_t`. The interrupt counter saturates
at `UINT64_MAX` and records overflow rather than wrapping backward. This gives
single-core readers a monotonic nondecreasing counter with explicit units and
an explicit failure result at the representable boundary.

TIME state uses a TIME-class irqsave lock. The Local APIC timer handler releases
that lock before calling `scheduler_timer_tick`, and bounded waits release it
while polling. The timer handler is the sole interrupt-owned TIME entry; public
TIME APIs must not be called from NMI, IST, or panic paths. This contract covers
maskable-interrupt serialization on the BSP only.

The bounded wait may temporarily enable interrupts only from an ordinary,
unpinned task context. Before taking the TIME lock or beginning its polling
window, it requires the current CPU to have both interrupt depth and preemption
count at zero. A caller inside an interrupt, an outer `preempt_guard`, or any
other critical pin is rejected without enabling interrupts and with `IF` still
clear. This prevents a nested critical section from becoming interruptible
behind its caller's back.

## Worst credible failure

The worst credible failure is a wrong calibration or invalid interrupt program
that makes time run at the wrong rate, stop, wrap, or enter an invalid vector.
Zenith treats every capability, sample, arithmetic, vector, mode, divisor,
initial-count, current-count, route-mask, and handler installation check as an
activation gate. Failure leaves `IF=0`, masks and clears the Local APIC timer,
stops and masks the PIT when the hardware accepts that cleanup, and returns a
specific status to the boot panic path. If a controller refuses a cleanup
write, activation still remains unpublished and the kernel halts with
interrupts disabled. There is no fallback clock whose silent use could hide a
bad rate.

## Executable proof

The pure rejection suites cover every divide encoding, reserved divide bits,
wrong sample counts, zero counts, excessive sample spread, frequency bounds,
periodic-count quantization, saturating increment, null outputs, zero periods,
and the largest successful and first overflowing nanosecond conversions.

The `lapic-timer` QEMU scenario traverses the existing APIC-routed PIT proof,
calibrates and activates the Local APIC timer, observes at least eight periodic
interrupts and EOIs, proves nanoseconds advance by the configured period, and
requires the PIT route to remain masked. The normal boot performs the same
integrated proof and emits:

```text
Zenith OS: Local APIC timer and monotonic clock verified
```

The host protocol now contains sixteen scenarios. The `heap` scenario also
forces one valid calibration attempt to be discarded, requires the bounded
retry diagnostic, and then proves periodic ticks and EOIs advance across
page-backed allocation, rollback, and exhaustion while the PIT route remains
masked. Each scenario requires one
exact `ZT BEGIN <scenario>` and one exact `ZT PASS <scenario>`, its
scenario-specific exit status, and no failure or panic marker. The scheduler
scenario additionally proves timer ticks and EOIs advance across voluntary and
involuntary switches, including a CPU-bound task that never yields.

## Deferred work

Zenith does not yet compensate for post-boot clock drift, use TSC-deadline,
HPET, an invariant TSC, one-shot deadlines, or a tickless design. Dynamic
vector allocation, tickless scheduling, SMP calibration and clock
synchronization, userspace clocks, and wall-clock time remain deferred.
