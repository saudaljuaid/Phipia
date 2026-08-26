<!-- SPDX-License-Identifier: GPL-3.0-only -->

## Change

What changed, and why it belongs in SapStudio.

## Rules

Which rules in `docs/ENGINEERING_RULES.md` this change is governed by, and — if
any rule is bent — the waiver that permits it.

## Evidence

- [ ] `make lint` passes from a clean tree.
- [ ] The gates required by `CONTRIBUTING.md` for this kind of change were run,
      and their exact commands are listed here.
- [ ] The negative control was performed, and the refusal it produced is quoted.
- [ ] No warning, failed check, or unexplained binary artefact is present.
- [ ] The commit is atomic, reviewable, and safe to revert.

## Risk

The most credible failure these checks do not cover, and the rollback plan.
"None" is not acceptable for anything touching the model, the platform
boundary, or a user's file.
