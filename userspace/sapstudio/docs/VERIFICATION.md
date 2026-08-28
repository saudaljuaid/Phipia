<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Verification

SapStudio separates host tests from checks of the freestanding Sapote image.

## Local gate

```sh
make lint
make check
make test
make image
make audit
make reproducible
make verify
```

- `make lint` checks repository hygiene, links, layering, and documented test
  totals.
- `make check` runs formatting and Clippy with warnings denied.
- `make test` runs the host workspace tests.
- `make image` links the freestanding target.
- `make audit` checks the resulting ELF shape and forbidden instructions.
- `make reproducible` compares two clean builds from separate directories.
- `make verify` runs the complete set.

## Test layers

### Model tests

Time, edits, overlap rules, undo/redo, transitions, masks, titles, markers,
retiming, and serialization are exercised without platform services.

### Media tests

Frame descriptions, colour conversion, compositing, rasterization, LUTs,
resampling, scopes, audio arithmetic, and the frame pool use small inputs with
independently calculated expected values where practical.

### Format tests

Readers reject truncation, invalid lengths, unsupported versions, checksum
failures, excessive dimensions, and trailing data. Round trips are paired with
tests that inspect the encoded bytes or use an independent reader.

### Runtime tests

The linked image must be static `ET_EXEC` at the configured address with no
dynamic section, relocations, undefined symbols, executable stack, W+X
segments, or SIMD instructions. The image is rebuilt twice for byte comparison.

## Failure checks

Critical tests should be exercised with an isolated mutation that reaches the
branch being protected. Examples include changing a checksum-covered byte,
removing an input from a cache key, weakening an overlap rule, swapping a
serialized field, or allowing a forbidden ELF feature.

The mutation is temporary. The useful record is the regression test and a
clear failure message, not a long narrative of every experiment.

## Fuzzing

Every shipping parser needs a bounded target and a committed seed corpus.
Crashes become minimized regression cases. Scheduled jobs may run longer, but
pull-request jobs remain short enough to provide routine feedback.

## Reference output

Reference renders include a hash and a viewable PNG where possible. A changed
reference must be reviewed as an intentional visual change. Golden output is
not used as a substitute for structural assertions about colour, geometry, or
file contents.

## Sapote integration

Host success does not establish that the program runs on Sapote. Native work
adds QEMU coverage for launch, input, framebuffer presentation, storage,
recovery, playback timing, and shutdown as those services become available.

## Pull requests and releases

Pull requests list the commands run and any relevant platform limitation.
Releases include the target image, source and licence records for vendored
dependencies, checksums, supported-format notes, and known limits.
