# Zenith OS engineering law

Kernel code does not get the benefit of a forgiving runtime. A plausible change
is not a correct change. These rules are mandatory.

## Commit gate

Every commit must be atomic, buildable, bootable, reviewable, and reversible.
Before creating a commit, run `make verify`. Before pushing an interrupt or CPU
state change, run `make qemu-tests`; other changes must run at least `make smoke`.
Enable and verify the repository hooks once with `make bootstrap`; disabling or
bypassing them is grounds to reject the change. The pre-commit gate rejects
unstaged or untracked files so `make verify` observes exactly the staged tree;
the pre-push gate checks each pushed ref tip in a temporary detached worktree.

No direct push to `main` is acceptable. Every change uses a pull request, whose
latest commit must pass all four verification checks:
`build, analyze, sanitize, inspect`,
`QEMU 16-scenario default RAM`,
`QEMU 19 MiB floor and 18 MiB OOM`, and
`QEMU scheduler competing-load repetitions`. Do not merge around a red,
cancelled, skipped, stale, or missing check. Do not force-push `main`, delete
`main`, or merge an unreviewed kernel change.

## Code standard

- The implementation languages are C11 and GNU assembly for the selected target.
- The kernel is freestanding: no host libc, hosted assumptions, or hidden runtime.
- Compiler and assembler warnings are errors. Suppression requires a written
  reason in the same change and must be narrower than the warning it addresses.
- Every hardware or ABI constant must name its source contract.
- Undefined behavior, unbounded waits, silent truncation, RWX mappings, and
  unexplained `volatile` or inline assembly are rejected.
- A new subsystem needs a documented invariant and an executable failure test.
- Generated binaries, ISO images, editor state, and local toolchains never enter
  version control.

## Review standard

The author must explain what changed, why it is necessary, its worst credible
failure mode, how it was tested, and how to revert it. Boot, memory-management,
interrupt, privilege, ABI, linker, and synchronization changes require focused
review; screenshots are not proof.

Review the code first, then the disassembly or binary layout when applicable,
then the QEMU serial transcript. A green CI run proves only the tested contract.
It does not prove that the design is correct.

## Commit format

Use an imperative subject of at most 72 characters, followed by the reason and
verification when useful. Keep unrelated formatting and refactors out of
functional commits. One logical change means one commit.

Examples:

```text
boot: enter x86_64 long mode
mm: reject overlapping physical regions
docs: record the interrupt-entry ABI
```

## Required repository settings

Protect `main` with pull requests, one approving review when another maintainer
is available, required code-owner review, dismissal of stale approvals, required
conversation resolution, linear history, signed commits, blocked force pushes
and deletions, and all four required verification checks named above.
Administrators do not bypass these rules.
