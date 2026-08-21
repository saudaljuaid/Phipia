# Sapote Boot Ledger

## The problem

Boot used to be correct because `kernel_main` called everything in the right
order. ACPI happened before topology consumers, the device-window registry
happened before paging, heap construction happened after W^X, and interrupts
were not enabled until the IDT and controllers existed. None of those facts was
machine-readable. A new call placed in the wrong part of one long function could
violate the contract without a structural refusal.

The Sapote Boot Ledger replaces that implicit order with a bounded runtime
contract. Each migrated stage has a stable `enum boot_stage_id`, a diagnostic
name, typed required and provided `enum boot_capability` values, required or
optional policy, a phase, an irreversible-ordering class and one typed execution
function. The complete plan is validated before any stage can program PAT,
replace CR3, enable interrupts, activate the local APIC timer, write the
framebuffer or activate the scheduler.

This is not formal verification. It is a bounded planner, execution ledger and
installed-state proof whose assertions run in the same kernel they check.

## Ownership and bounds

`include/sapote/boot_ledger.h` is the public vocabulary. The bounds are:

| Object | Capacity |
| --- | ---: |
| descriptors and canonical plan | 34 stages |
| receipts | 34 receipts |
| required, success or fallback capabilities per descriptor | 11 each |
| stable proof counters per receipt | 2 |

Plan construction, validation, execution bookkeeping and the pure planner test
use fixed storage. They cannot allocate from the heap whose construction the
plan authorizes. A compile-time assertion keeps the capability enumeration
within the private planner's bounded representation; public interfaces expose
typed capability arrays and accessors, never undocumented bit positions.

The loader record formerly called `struct boot_context` is now the narrower
`struct boot_information`. `struct boot_context` owns the lifetime-wide state:
the parsed loader record, ACPI tables and topology, device-window registry,
optional MCFG decision and scenario context. One statically allocated instance
outlives the 16 KiB boot stack and the interactive shell. It owns values, not
untyped pointers. `kernel_main` initializes it, descriptor functions populate
it after their requirements have receipts, and later stages consume those
fields. The published ledger is read-only after installed verification.

## Installed stages

The table describes the normal canonical plan. A dash means the stage provides
no new public capability; it still receives one ordered receipt.

| ID | Stage | Requires | Provides | Policy | Phase / irreversible class |
| ---: | --- | --- | --- | --- | --- |
| 1 | early serial | — | early serial available | required | foundation |
| 2 | interrupt foundation | early serial | IDT installed | required | foundation |
| 3 | pure boot self-tests | serial, IDT | boot self-tests complete | required | foundation |
| 4 | boot information | self-tests | boot information validated | required | discovery |
| 5 | firmware discovery | boot information | ACPI root, interrupt topology, clocks discovered | required | discovery |
| 6 | device-window registry | boot information, topology | registry validated, framebuffer availability decided | required | discovery |
| 7 | interrupt controllers | ACPI root, topology, clocks, registry | interrupt controllers configured | required | controllers |
| 8 | physical frame allocator | boot information, controllers | frame allocator available | required | controllers |
| 9 | PAT and page-table installation | registry, frame allocator | page tables installed | required | memory transition / PAT+CR3 |
| 10 | installed paging proofs | page tables | W^X proved, installed windows proved | required | memory transition |
| 11 | independent framebuffer WC proof | page tables, installed windows, framebuffer decision | WC independently proved; serial fallback when skipped | optional | memory transition |
| 12 | heap and paging runtime | page tables, W^X | heap available | required | runtime |
| 13 | framebuffer output | independent WC, heap | surface available, framebuffer output installed | optional | runtime / framebuffer output |
| 14 | keyboard interrupt path | IDT, controllers, heap | interrupts enabled, keyboard available | required | runtime / interrupt enable |
| 15 | interactive shell | surface, interrupts | shell available | optional | runtime |
| 24 | First Light UI font | surface | UI font verified | optional | runtime |
| 25 | pointer availability decision | keyboard, controllers | pointer availability decided | optional | runtime |
| 26 | pointer availability outcome | pointer decision | pointer input available; declared absence on neutral skip | optional neutral | runtime |
| 27 | First Light layout | surface, UI font | UI layout validated | optional | runtime |
| 16 | early scenario gate | heap, interrupts | — | required | runtime |
| 17 | interrupt proofs | IDT, controllers, interrupts | — | required | timers |
| 18 | interrupt routing | IDT, controllers, interrupts | interrupt routing proved | required | timers |
| 19 | timer calibration | IDT, controllers, interrupts, clocks, routing | timer calibration complete | required | timers / APIC timer |
| 20 | PCI access | page tables, timer calibration | PCI access available | required | services |
| 21 | threading | heap, timer calibration | threading available | required | services |
| 22 | scheduler | heap, threading, timer calibration, interrupts | scheduler available | required | services / scheduler |
| 31 | PCI resource ownership | paging, PCI, frames | PCI resource ownership available | required | services |
| 32 | dynamic interrupt vectors | IDT, controllers, local APIC, interrupts | dynamic vector foundation available | required | services |
| 33 | DMA foundation | paging, frames | DMA foundation available | required | services |
| 34 | installed device-substrate proof | paging, PCI, heap, frames, local APIC, interrupts, threading, scheduler, PCI resources, vectors, DMA | installed device-substrate proof; declared fixture absence on neutral skip | optional neutral | services |
| 23 | closing boot proofs | page tables, installed windows, heap, PCI, scheduler | boot proofs complete | required | proofs |
| 28 | desktop construction | surface, UI font, layout, pointer decision | desktop shell available | optional | proofs |
| 29 | desktop activation | desktop, framebuffer output, WC, surface, font, layout, keyboard, threading, scheduler, closing proofs | desktop shell activated | optional | proofs |
| 30 | First Light installed proof | activated desktop, WC, closing proofs | First Light installed proof complete | optional | proofs |

`install_page_tables` remains one indivisible execution function. It preserves
the existing PAT MSR read/program/readback sequence and the existing
`WBINVD`, CR3 replacement, `WBINVD` sequence. Stage 10 adds a later semantic
receipt by freshly walking the installed hierarchy; it does not split or replay
the transition.

## Capabilities

The complete capability enumeration is:

1. early serial available;
2. boot self-tests complete;
3. boot information validated;
4. physical frame allocator available;
5. ACPI root validated;
6. interrupt topology discovered;
7. clocks discovered;
8. device-window registry validated;
9. page tables installed;
10. W^X proved;
11. installed device windows proved;
12. heap available;
13. IDT installed;
14. interrupt controllers configured;
15. interrupts enabled;
16. interrupt routing proved;
17. timer calibration complete;
18. PCI access available;
19. framebuffer availability decided;
20. framebuffer WC independently proved;
21. serial framebuffer fallback;
22. surface available;
23. threading available;
24. scheduler available;
25. shell available;
26. boot proofs complete;
27. framebuffer output installed;
28. keyboard available;
29. UI font verified;
30. pointer availability decided;
31. pointer input available;
32. pointer input absent;
33. UI layout validated;
34. desktop shell available;
35. desktop shell activated;
36. First Light installed proof complete;
37. PCI resource ownership available;
38. dynamic vector foundation available;
39. DMA foundation available;
40. device-substrate installed proof complete; and
41. device-substrate fixture absent.

The four appended identifiers preserve every pre-v0.3.0 stable stage and
capability number. Their declared phase and requirements place all three
foundations and the proof after the scheduler but before closing proofs; raw
enum position and descriptor declaration order do not define execution order.

The framebuffer decision and framebuffer WC proof are intentionally distinct.
The device-window registry says what paging installed. The independent WC stage
re-derives the framebuffer span from validated boot information, verifies PAT
readback, walks every page and separately checks that VGA, APIC, ECAM and normal
RAM retained their UC or WB types. The registry is not the sole WC oracle.

## Canonical planning

Validation first checks identifiers, enum bounds, per-stage capacities,
duplicate stage IDs, duplicate requirements and exclusive capability providers.
Every requirement must have exactly one declared provider. A provider in a
later phase is an invalid phase transition.

The planner then performs a bounded topological selection. Among ready stages,
the lower phase wins; within one phase the lower stable stage ID wins. Raw
descriptor insertion order is never consulted as a tie-break. If no unplanned
stage is ready, the lowest stable ID still blocked names the dependency cycle.
Two-stage and longer cycles are therefore the same named refusal. A raw
declaration permutation produces the same canonical IDs, receipts, capability
transitions and fingerprint.

Optional stages still have providers in the validated graph. At execution, an
optional stage whose runtime requirements were not established receives a
skipped receipt without being called. A stage that runs and reports optional
failure receives a failed receipt. Neither path can mint its success
capabilities. A narrowly declared neutral skip can establish an absence
capability without degrading the ledger; the pointer outcome uses it after the
decision stage. The WC stage may instead provide its separately declared serial
fallback capability when the framebuffer is absent. A required skip is always
a refusal. A required failure appends its failure receipt and stops execution;
no dependent or later stage runs.

## Irreversible ordering

The phase graph is supplemented by semantic irreversible classes. Validation
refuses a descriptor that does not declare all mandatory prerequisites:

| Class | Mandatory capabilities before execution |
| --- | --- |
| PAT programming, `WBINVD` and CR3 replacement | validated device windows, physical frames |
| interrupt enable | IDT, configured interrupt controllers |
| framebuffer output | independent framebuffer WC proof |
| local APIC timer activation | IDT, controllers, interrupt-enable receipt, discovered clocks |
| scheduler activation | heap, threads, timer calibration, interrupt-enable receipt |

Desktop activation is not an irreversible-class member, but installed semantic
verification requires its exact ten prerequisites. In particular, it rejects
removal of the independent WC or scheduler edge by naming desktop activation
and the missing capability, even if phases happen to preserve observed order.

The device-substrate proof similarly is not an irreversible-class member, but
its descriptor must carry exactly eleven prerequisites: paging, PCI, heap,
frames, local APIC/controllers, interrupts, threading, scheduler, and all three
new foundations. Its execution function both shortens a local copy and replaces
one member while preserving cardinality; the semantic prerequisite checker must
reject both. The installed verifier
then requires either the ran receipt with counters `1` interrupt and `64` DMA
bytes, or a neutral skipped receipt with the fixture-absent capability, exactly
one and never neither or both. Closing proofs independently require every claim,
MMIO page, DMA handle,
vector, handler, MSI-X binding, and bus-master count to be zero.

The framebuffer and cached-surface store fences remain in their existing
implementation paths. The ledger changes who may call those paths, not their
ordering instructions.

## Receipts and fingerprint

Every canonical stage produces at most one bounded `struct boot_stage_receipt`.
It records the stable stage ID, one-based canonical sequence, result class
(`ran`, `skipped` or `failed`), ledger status, typed capabilities actually
established and up to two stable proof counters. It records neither time nor a
machine address. A successful result must exactly match its descriptor's
success capability set. A skip must exactly match its declared fallback set. A
failure must provide nothing.

The summary's executed count includes stages whose functions ran and either
succeeded or failed; it excludes stages skipped without execution. The separate
skip count includes only optional skip receipts.

The 64-bit fingerprint is FNV-1a with an explicit format version. It covers the
canonical stage IDs, required/optional policy, phases, irreversible classes,
sorted typed requirement/success/fallback sets, then every receipt's stage ID,
sequence, result class, status and sorted capability transition. Proof counters
are excluded because they may describe a particular machine. Reordering raw
descriptors cannot change it; changing a stable stage ID or receipt does.

This fingerprint is a deterministic integrity and debugging summary. FNV-1a is
not collision-resistant. The value is not cryptographic attestation, is not a
security boundary and must not be used as one.

## Refusals

`enum boot_ledger_status` has a complete compile-time-checked string table. Its
named results are:

- ok;
- null argument;
- empty boot plan;
- boot plan not validated;
- boot plan already executed;
- unknown stage identifier;
- unknown capability;
- too many stages;
- too many receipts;
- too many stage capabilities;
- duplicate stage;
- duplicate capability requirement;
- duplicate capability provider;
- missing capability provider;
- capability dependency cycle;
- invalid phase transition;
- irreversible stage ordered too early;
- required stage skipped;
- undeclared capability provided;
- stage executed before its requirements;
- required stage failed;
- optional stage failed;
- optional stage skipped;
- receipt mismatch;
- plan fingerprint mismatch; and
- installed plan differs from validated plan.

A refusal stores the offending typed stage and capability whenever one exists.
Boot diagnostics print both semantic names plus a subsystem detail for an
execution failure.

## Pure and installed proofs

`boot_ledger_self_test` uses synthetic fixed-storage plans before any PAT or CR3
transition. It accepts a mixed required/optional plan; permutes declarations;
rejects missing and duplicate providers, duplicate stages and requirements,
unknown enums, two-stage and longer cycles and capacity plus one; exercises
undeclared capability minting and execution before requirements; proves
optional failure leakage and required-failure stopping; compares fingerprints;
changes one stable ID; and accepts framebuffer/ECAM absence.

After the normal plan finishes, `boot_ledger_verify_installed` walks the plan
and receipts again. It proves canonical order, one receipt per stage, exactly
one provider per established capability, requirements before consumers,
required success, optional non-leakage, known stage IDs and the recomputed
fingerprint. It then re-walks paging and the installed device windows, repeats
the W^X audit, and checks active framebuffer output has both independent WC and
surface receipts. Interrupt, APIC-timer and scheduler ordering follows from the
same mandatory typed prerequisites. When First Light completes, verification
also checks the font receipt and metrics, mutually exclusive pointer outcome,
layout/construction/activation/proof receipts, WC and closing-proof order,
installed UI geometry and the recomputed render receipt. The permanent line is:

    Sapote: Boot Ledger installed proof passed

The normal transcript contract requires that line, so deleting the proof from
permanent boot output is a host-test failure.

## Operator and scenario interfaces

At `sap>`, `ledger` prints a bounded summary with no machine addresses:

    boot ledger :: PASS
    plan 34  run 33  skip 1  caps 38  receipts 34
    fingerprint 0xHHHHHHHHHHHHHHHH

The exact fingerprint is build-plan dependent. Fixture and pointer absence use
declared neutral-skip capabilities and remain `PASS`; other optional fallback or
failure changes the state to `DEGRADED`.

The `boot-ledger` QEMU scenario owns guest exit `0x2E`, host status 93. It emits
exactly one begin and pass marker, walks the actual published ledger, checks all
mandatory receipts and every dependency edge, checks device windows before
paging and installed W^X/window proofs after it, checks IDT/controllers before
interrupt enable, checks conditional WC before framebuffer output, verifies the
fingerprint and prints this numeric shape:

    ST LEDGER stages N receipts N capabilities N skips N fingerprint 0xHHHHHHHHHHHHHHHH

## Negative controls

Each control was run from a file snapshot, one mutation at a time, after a clean
build; the narrowest scenario was used and the snapshot was restored without
`git checkout -- <file>`.

| Mutation | Required observation | Result |
| --- | --- | --- |
| remove device-window provider | named missing-provider refusal before PAT/CR3 | PASS — `missing capability provider`; `interrupt controllers`; `device-window registry validated` |
| make paging require heap | named dependency cycle | PASS — `capability dependency cycle`; `PAT and page-table installation` |
| duplicate a stable stage ID | named duplicate-stage refusal | PASS — `duplicate stage`; `early serial` |
| duplicate one exclusive capability provider | named duplicate-provider refusal | PASS — `duplicate capability provider`; `device-window registry`; `IDT installed` |
| add capacity plus one descriptors | named stage-capacity refusal | PASS — `too many stages` before execution |
| reverse descriptor declarations | identical order, receipts, fingerprint and boot | PASS — canonical receipt order and guest status remain identical |
| remove one of the device proof's eleven prerequisites | semantic prerequisite refusal before touching the fixture | PASS — local descriptor copy is rejected on every boot |
| change device-substrate guest exit `0x30` | exit-contract negative control | PASS — mutated `0x31` is rejected; host contract remains 97 |
| execute paging before device-window validation | stage/capability precondition refusal | PASS — `stage executed before its requirements`; paging stage; registry capability |
| move interrupt enable before IDT | irreversible-order refusal | PASS — `irreversible stage ordered too early`; keyboard stage; `IDT installed` |
| allow framebuffer output before WC | framebuffer/WC refusal | PASS — `irreversible stage ordered too early`; framebuffer output; independent WC capability |
| fail optional framebuffer WC stage | serial boot continues; WC success absent | PASS — installed proof completed, WC and surface success stayed absent, and serial reached `sap>` |
| fail required paging stage | no heap, interrupt, framebuffer or scheduler dependent runs | PASS — named required paging failure; no dependent transcript lines ran |
| falsify one installed receipt | installed proof names the stage | PASS — `installed plan differs from validated plan`; paging stage |
| change one stage ID after validation | plan fingerprint mismatch | PASS — `plan fingerprint mismatch` before execution |
| call a migrated operation outside its descriptor file | `make verify` source assertion fails | PASS — assertion named the bypassing source line |
| delete permanent ledger proof line | normal transcript assertion fails | PASS — comparator showed the one deleted proof line |

The transcript comparator was also challenged by changing one permanent word
and by changing the existing `normal` guest exit from `0x10` to `0x11`. It
rejected both mutations as well as the proof-line deletion.

## Limitations and deferred work

- Stage implementations inherited from `boot_proofs.c` still use fatal proof
  helpers internally. The descriptor boundary prevents bypass and the engine
  records returned stage failures, but a deep proof panic halts at its original
  diagnostic rather than unwinding C control flow.
- The plan is single-core and immutable after publication. There is no dynamic
  module loading, hot-plug or live MMIO remapping.
- Optional framebuffer output has a serial fallback and pointer input has a
  first-class neutral absence receipt. Legacy PCI port access still covers
  missing ECAM without another absence stage.
- The fingerprint summarizes integrity for debugging only. Cryptographic
  measurement and remote attestation are outside this design.
- The ledger does not add userspace, SMP scheduling, MTRR programming,
  higher-half conversion or display-driver work.
