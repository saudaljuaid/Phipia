<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Contributing to SapStudio

SapStudio is a Sapote application. Changes stay narrow, preserve the trust
boundaries, and arrive with evidence that matches their risk.

Read [`docs/ENGINEERING_RULES.md`](docs/ENGINEERING_RULES.md) before your first
change. It is normative, and review will cite it by rule number.

## Set up

```sh
rustup target add x86_64-unknown-none
make hooks     # enable the pre-commit check for this clone
make verify    # everything this repository can currently prove
```

`make hooks` sets `core.hooksPath`, which is local configuration and therefore
not carried by a clone. Run it, and run it again on every fresh clone. It is
one line and it is the difference between finding a hygiene failure before the
commit and finding it in CI: this repository has had a commit pushed that
`make lint` refuses, because the hook was not installed and the shell command
that was supposed to gate the push had the gate and the push as separate
statements.

Ubuntu 24.04 or a compatible Debian system is the reference host, as it is for
Sapote, and the same GNU binutils build both. The pinned compiler is in
`rust-toolchain.toml`.

## Work on a branch

Start from the current remote default branch and use a descriptive name:

```sh
git fetch origin
git switch -c timeline-ripple-trim origin/main
```

Do not push directly to `main`, bypass hooks, or force-push shared history.

## Required evidence

| Change | Minimum evidence |
| --- | --- |
| Documentation only | `make lint` |
| Rules, policy, or platform contract | `make lint`, plus the affected documents updated in the same change |
| Ordinary code | `make lint`, `make check`, and `make test` |
| A parser, a format, or anything reading bytes | Above, plus a fuzz target and a committed corpus |
| Render, colour, or audio path | Above, plus golden hashes and a determinism check |
| Platform boundary, allocator, or ABI | Above, plus `make verify` and, once they exist, the relevant QEMU scenarios |
| A new dependency | The complete import gate in [`docs/DEPENDENCY_POLICY.md`](docs/DEPENDENCY_POLICY.md) |

Every change that claims an invariant performs the negative control described
in [`docs/VERIFICATION.md`](docs/VERIFICATION.md) and reports the refusal it
produced. An invariant no test can break is not an invariant.

**A change that adds or removes a test updates the counts in the same commit.**
`docs/ARCHITECTURE.md` states a count per crate and `README.md` states a total
and a count of negative controls, and `make lint` refuses when any of them
disagrees with the tree. That is friction on purpose: those numbers were kept
by hand for the first five hundred tests, and a number kept by hand is a number
that goes stale — which in a document whose whole claim is that it does not
overstate is worse than having no number at all.

## Commits

One logical change per commit. Imperative subject, at most 72 characters,
prefixed by area:

```text
timeline: refuse an edit whose out point precedes its in point
abi: assert the frame descriptor layout on both sides
docs: record the large code model's section renaming
```

## Pull requests

State what changed and why it belongs in SapStudio; the exact verification run;
the negative control and its refusal; and the most credible failure the checks
do not cover. "None" is not an acceptable risk statement for anything that
touches the model, the platform, or a user's file.

## What will be refused

A change that widens a bounded contract instead of creating a new one; that
adds a dependency outside the gate; that introduces `unsafe` outside the two
crates permitted to hold it; that adds a portability layer or a POSIX
assumption; that makes a render non-deterministic; or that could lose a user's
work. These are not review preferences. They are
[the rules](docs/ENGINEERING_RULES.md).
