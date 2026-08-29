<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Native userspace performance diagnostics

Sapote records bounded diagnostics to catch structural regressions. They are
QEMU TCG measurements, not production throughput claims, and the QEMU serial
logs retained by the native-porting workflow are the evidence of record.

| Marker | Measured boundary |
| --- | --- |
| `SAPOTE PERF syscall` | 1,024 Ring 3 ABI-version queries, reported as total and mean monotonic nanoseconds. |
| `SAPOTE PERF file` | 64 KiB sequential Data write and read in 4 KiB requests through native file handles. |
| `SAPOTE PERF context-switch` | Real scheduler transition sections. `without_fpu_cycles` includes saved GPR context, address-space activation/restoration, and FS-base work; `with_fpu_cycles` adds the eager aligned `FXSAVE64` or `FXRSTOR64` section. Both are per-transition TSC means and exclude application work. |
| `SAPOTE PERF canvas` | A 70×14 xRGB8888 damage submission from each of two concurrent native Canvas processes. The app reports sample count, total, and mean monotonic nanoseconds. |
| `SAPOTE PERF lua` | Native entry-probe time through upstream Lua state creation and standard-library initialization. A link wrapper observes `luaL_openlibs` without changing upstream sources. |
| `SAPOTE PERF sqlite` | The committed insert transaction in phase one, then database reopen, query, integrity check, and close after a clean reboot. |

`make qemu-port-tests` requires every marker to be present and nonzero. The
same run rejects missing partial-damage activity, absent guest output files,
resource leaks, or an invalid reboot result. File and syscall diagnostics use
public ABI calls from Ring 3; the scheduler comparison is recorded inside the
security boundary because userspace cannot read kernel transition sections.

The Canvas proof also retains a PNG and short MP4 captured directly with QEMU
`screendump` while both native windows are alive. No host-rendered substitute
or camera path participates in that evidence.
