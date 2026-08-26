<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Dependency policy

[`DEPENDENCIES.md`](DEPENDENCIES.md) says what SapStudio might use. This says
how something actually gets in, what it costs to keep, and how it leaves.

A dependency is a permanent liability with a temporary benefit. The gate below
is deliberately expensive so that the benefit has to be real.

## The import gate

A dependency enters only through a pull request that does all nine of these, in
one reviewable change.

**1. State the need.** One paragraph: what SapStudio cannot do without it, why
the need is real now rather than anticipated, and what the code looks like
without it. "It would be convenient" fails here.

**2. State the alternative.** Name the shortest path to writing it here, with
an estimate. If that estimate is under a week and the component is core
semantics, R-12.7 refuses the import.

**3. Verify the licence from the source.** Read the licence file in the
vendored tree. Check every subdirectory: media libraries routinely carry a
different licence for a test corpus, an assembly file, or one bundled
dependency. Record the SPDX expression and the file paths that establish it. A
registry field is not evidence.

**4. Verify the shape.** The component must build for the SapStudio target with
no `std`, no libc, no build script that generates code from an uncommitted
source, and no network access. Record the exact build command.

**5. Count the `unsafe`.** Record the number of `unsafe` blocks and their
purpose in one line each. This is the dependency's budget, and it is compared
at every upgrade.

**6. Vendor it.** `vendor/<name>-<version>/`, the complete source, no
submodule, no fetch at build time. Record the upstream URL, the exact revision,
and the SHA-256 of the archive. Two clean builds must produce identical
artefacts (R-13.2).

**7. Wrap it.** No dependency's types appear in SapStudio's own code. Each one
gets a wrapper module that translates its errors into SapStudio's typed status
enum, bounds its inputs, and refuses what R-1.1 requires bounding. For a C or
C++ dependency, that wrapper lives in `sapstudio-abi` and nowhere else.

**8. Test it.** A test that proves the wrapper's contract, and a negative
control that proves the wrapper refuses what it claims to refuse. For any
dependency that parses bytes, a fuzz target as well (R-11.3).

**9. Record it.** An entry in `deps/manifest.toml` with every field below, and
a row updated in [`DEPENDENCIES.md`](DEPENDENCIES.md).

## The manifest

`deps/manifest.toml` is the authoritative record. CI checks that the manifest,
the vendored tree, and the lockfile agree.

```toml
[[dependency]]
name          = "tiny-skia"
version       = "0.11.4"
upstream      = "https://github.com/RazrFalcon/tiny-skia"
revision      = "<exact git revision>"
archive_sha256 = "<uppercase hex>"
licence       = "BSD-3-Clause"
licence_files = ["LICENSE", "src/pipeline/LICENSE-skia"]
tier          = "T1"
purpose       = "CPU path rasterisation for the compositor and interface."
alternative   = "Write a rasteriser; roughly two months for lower quality."
owner         = "<person accountable for this dependency>"
unsafe_blocks = 0
std_required  = false
build_script  = false
wrapper       = "crates/sapstudio-render/src/raster/mod.rs"
fuzz_target   = "fuzz/targets/raster_path.rs"
exit_plan     = "Swap to `zeno` behind the same wrapper trait; two weeks."
imported      = "<date>"
reviewed      = "<date>"
```

Every field is required. `exit_plan` is not decoration: a dependency without a
credible answer to "it was abandoned this morning" is a dependency that owns
this project rather than the other way round.

## Keeping it

**Review annually.** Each dependency's `reviewed` date is checked in CI. A
record older than a year fails the build until someone re-reads the licence,
re-counts the `unsafe`, and confirms the component is still maintained and
still needed.

**Upgrades are deliberate.** One dependency per commit, with the upstream
changelog summarised, the `unsafe` count re-taken, the licence re-verified, and
the full evidence run. Automated dependency updates are forbidden (R-12.6).

**Pinning is absolute.** Exact versions, a committed lockfile, `--locked
--offline` in every build. A build that can resolve a different version than
the last one is not reproducible and therefore not acceptable (R-13.2).

**Duplicates are refused.** Two crates that do the same job is a decision that
was never made. `cargo-deny`'s duplicate check runs with `deny`.

## Extra rules for C and C++ dependencies

A native dependency costs more than a Rust one and must earn the difference.

- It is wrapped in `sapstudio-abi` and never appears elsewhere (R-3.2.1).
- Its build is reproduced by SapStudio's own build rules. Its upstream build
  system is not run, because it will look for a libc, a configure script, and a
  host it does not have. Where that is impractical, the port is the work item,
  not a shortcut around it.
- It is compiled with SapStudio's flags, including warnings-as-errors and the
  SIMD prohibition of R-13.6.
- Every buffer it writes into is validated over its complete range by Rust
  before the call (R-3.2.5).
- Its assembly is audited for instructions the platform does not support, using
  the same disassembly scan Sapote runs on BusyBox.
- It is presumed hostile: R-11 applies to it exactly as it applies to a file.

## Removal

Removing a dependency is a normal, welcome change. It requires: the code that
replaces it, the manifest entry deleted, the vendored tree deleted, the
`DEPENDENCIES.md` row moved to a verdict of `Reject` with the reason, and
evidence that nothing regressed.

A dependency is removed on sight when it: changes to an incompatible licence,
gains a network or telemetry path, gains an unbounded allocation in a path
SapStudio uses, is unmaintained with an open memory-safety issue, or turns out
to duplicate something the application must own.

## What CI enforces

| Check | Tool |
| --- | --- |
| Licence allowlist, advisories, duplicates, unknown sources | `cargo-deny` |
| Lockfile is current and the build is offline | `cargo --locked --offline` |
| Vendored tree matches the manifest digests | `tools/check-vendor.py` |
| Manifest is complete and every review date is current | `tools/check-manifest.py` |
| `unsafe` count per dependency matches the recorded budget | `cargo-geiger` |
| Every file carries an SPDX identifier | `reuse` |
| Two clean builds are byte-identical | the build itself |

The tools named here are written at the milestone that first needs them, and
are listed now so that no dependency arrives before the gate that judges it.
