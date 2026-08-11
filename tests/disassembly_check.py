#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Reject floating-point and vector instructions in kernel executable code."""

from __future__ import annotations

import argparse
import re
import subprocess


INSTRUCTION = re.compile(
    r"^\s*[0-9a-f]+:\s+([a-zA-Z][a-zA-Z0-9_.]*)(?:\s+(.*))?$"
)
SYMBOL = re.compile(r"^\s*[0-9a-f]+\s+<([^>]+)>:$")
VECTOR_REGISTER = re.compile(
    r"(?<![a-zA-Z0-9_])%?(?:xmm|ymm|zmm|mm)[0-9]+(?![a-zA-Z0-9_])"
    r"|(?<![a-zA-Z0-9_])%?st(?:\([0-7]\))?(?![a-zA-Z0-9_])"
)
STANDALONE_VECTOR = {
    "emms",
    "femms",
    "ldmxcsr",
    "stmxcsr",
    "vzeroall",
    "vzeroupper",
    "wait",
    "xgetbv",
    "xrstor",
    "xrstor64",
    "xrstors",
    "xrstors64",
    "xsave",
    "xsave64",
    "xsavec",
    "xsavec64",
    "xsaveopt",
    "xsaveopt64",
    "xsaves",
    "xsaves64",
    "xsetbv",
}

EXPECTED_XSTATE_INSTRUCTIONS = {
    "xstate_arch_xgetbv": (("xgetbv", ""),),
    "xstate_arch_xsetbv": (("xsetbv", ""),),
    "xstate_arch_save": (("xsave64", "(%rdi)"),),
    "xstate_arch_restore": (("xrstor64", "(%rdi)"),),
    "scheduler_test_timer_register_probe": (
        ("movq", "%rax,%xmm0"),
        ("movq", "%rbx,%xmm7"),
        ("movq", "%r15,%xmm15"),
        ("movq", "%xmm0,%rdx"),
        ("movq", "%xmm7,%rdx"),
        ("movq", "%xmm15,%rdx"),
    ),
    "scheduler_test_xstate_clobber": (
        ("pxor", "%xmm0,%xmm0"),
        ("pcmpeqb", "%xmm7,%xmm7"),
        ("movq", "%rax,%xmm15"),
    ),
    "scheduler_nm_fault_site": (
        ("pxor", "%xmm0,%xmm0"),
    ),
}

RESTORE_POP_ORDER = (
    "%r15",
    "%r14",
    "%r13",
    "%r12",
    "%r11",
    "%r10",
    "%r9",
    "%r8",
    "%rsi",
    "%rdi",
    "%rbp",
    "%rdx",
    "%rcx",
    "%rbx",
    "%rax",
)


def normalized_operands(operands: str) -> str:
    return operands.replace(" ", "")


def sized_mnemonic_is(mnemonic: str, base: str) -> bool:
    return mnemonic in {base, f"{base}q"}


def nm_trigger_sets_ts(instructions: list[tuple[str, str]]) -> bool:
    """Require the #NM probe to set CR0.TS immediately before its fault site."""

    if len(instructions) != 3:
        return False
    first, second, third = instructions
    return (
        sized_mnemonic_is(first[0], "mov")
        and normalized_operands(first[1]) == "%cr0,%rax"
        and sized_mnemonic_is(second[0], "or")
        and normalized_operands(second[1]) in {"$0x8,%rax", "$8,%rax"}
        and sized_mnemonic_is(third[0], "mov")
        and normalized_operands(third[1]) == "%rax,%cr0"
    )


def expected_xstate_instruction(
    symbol: str | None, mnemonic: str, operands: str
) -> bool:
    if symbol not in EXPECTED_XSTATE_INSTRUCTIONS:
        return False
    return (mnemonic, normalized_operands(operands)) in {
        (expected_mnemonic, normalized_operands(expected_operands))
        for expected_mnemonic, expected_operands in
        EXPECTED_XSTATE_INSTRUCTIONS[symbol]
    }


def rsp_add_is(instruction: tuple[str, str], amount: int) -> bool:
    mnemonic, operands = instruction
    if not sized_mnemonic_is(mnemonic, "add"):
        return False
    normalized = normalized_operands(operands)
    if not normalized.endswith(",%rsp") or not normalized.startswith("$"):
        return False
    try:
        return int(normalized[1 : -len(",%rsp")], 0) == amount
    except ValueError:
        return False


def interrupt_entry_error(instructions: list[tuple[str, str]]) -> str | None:
    """Prove the common entry has one linear dispatch-to-iretq return path."""

    dispatch_calls = [
        index
        for index, (mnemonic, operands) in enumerate(instructions)
        if mnemonic.startswith("call") and "<interrupt_dispatch>" in operands
    ]
    all_calls = [
        index
        for index, (mnemonic, _) in enumerate(instructions)
        if mnemonic.startswith("call")
    ]
    if len(dispatch_calls) != 1 or all_calls != dispatch_calls:
        return "interrupt common entry must call only interrupt_dispatch, exactly once"

    # This Assembly routine is intentionally straight-line.  Reject every jump
    # or alternate return anywhere in the symbol, including one placed before
    # the dispatch call that could bypass the validated RSP handoff.
    for index, (mnemonic, _) in enumerate(instructions):
        if mnemonic.startswith("j") or mnemonic.startswith("loop"):
            return "interrupt common entry must not contain branches"
        if (
            mnemonic.startswith("ret")
            or mnemonic.startswith("lret")
            or mnemonic.startswith("sysret")
            or mnemonic == "sysexit"
            or (mnemonic.startswith("iret") and index != len(instructions) - 1)
        ):
            return "interrupt common entry must not contain an alternate return"

    dispatch_call = dispatch_calls[0]
    epilogue = instructions[dispatch_call + 1 :]
    expected_length = 1 + 1 + len(RESTORE_POP_ORDER) + 1 + 1
    if len(epilogue) != expected_length:
        return "interrupt common entry must have the exact validated restore epilogue"

    restore_mnemonic, restore_operands = epilogue[0]
    if (
        not sized_mnemonic_is(restore_mnemonic, "mov")
        or normalized_operands(restore_operands) != "%rax,%rsp"
    ):
        return "interrupt common entry must restore RSP directly from dispatch RAX"
    if not rsp_add_is(epilogue[1], 8):
        return "interrupt common entry must discard exactly the saved CR2 slot"

    for instruction, register in zip(
        epilogue[2 : 2 + len(RESTORE_POP_ORDER)], RESTORE_POP_ORDER
    ):
        mnemonic, operands = instruction
        if (
            not sized_mnemonic_is(mnemonic, "pop")
            or normalized_operands(operands) != register
        ):
            return "interrupt common entry must restore every GPR in ABI order"

    vector_discard = 2 + len(RESTORE_POP_ORDER)
    if not rsp_add_is(epilogue[vector_discard], 16):
        return "interrupt common entry must discard exactly vector and error slots"
    if epilogue[-1] != ("iretq", ""):
        return "interrupt common entry must end immediately in iretq"
    return None


def interrupt_entry_internal_self_test() -> None:
    epilogue = [
        ("mov", "%rax,%rsp"),
        ("add", "$0x8,%rsp"),
        *(("pop", register) for register in RESTORE_POP_ORDER),
        ("add", "$0x10,%rsp"),
        ("iretq", ""),
    ]
    valid = [("cld", ""), ("call", "1000 <interrupt_dispatch>")] + epilogue
    if interrupt_entry_error(valid) is not None:
        raise RuntimeError("valid interrupt epilogue failed its internal self-test")

    invalid_cases = (
        valid[:1] + [("jne", "2000 <interrupt_common_entry+0x20>")] + valid[1:],
        [("retq", "")] + valid,
        [("iretq", "")] + valid,
        valid[:3] + [("jmp", "3000 <interrupt_common_entry+0x40>")] + valid[3:],
        valid[:-1] + [("retq", "")],
        valid + [("iretq", "")],
    )
    if any(interrupt_entry_error(case) is None for case in invalid_cases):
        raise RuntimeError("unsafe interrupt epilogue passed its internal self-test")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("objdump")
    parser.add_argument("elf")
    arguments = parser.parse_args()
    if (
        VECTOR_REGISTER.search("jne <scheduler_self_test>") is not None
        or VECTOR_REGISTER.search("mov %xmm4,%rax") is None
        or VECTOR_REGISTER.search("fstp st(3)") is None
    ):
        parser.error("internal vector-register recognition self-test failed")
    interrupt_entry_internal_self_test()
    completed = subprocess.run(
        [arguments.objdump, "-d", "--no-show-raw-insn", arguments.elf],
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.returncode != 0:
        parser.error(completed.stderr.strip() or "objdump failed")

    rejected: list[str] = []
    observed_xstate: dict[str, list[tuple[str, str]]] = {
        symbol: [] for symbol in EXPECTED_XSTATE_INSTRUCTIONS
    }
    current_symbol: str | None = None
    interrupt_common_entry: list[tuple[str, str]] = []
    nm_trigger_instructions: list[tuple[str, str]] = []
    for line in completed.stdout.splitlines():
        symbol_match = SYMBOL.match(line)
        if symbol_match is not None:
            current_symbol = symbol_match.group(1)
            continue

        match = INSTRUCTION.match(line)
        if match is None:
            continue
        mnemonic = match.group(1).lower()
        operands = (match.group(2) or "").lower()
        if current_symbol == "scheduler_test_trigger_nm":
            nm_trigger_instructions.append((mnemonic, operands))
        if current_symbol == "interrupt_common_entry":
            interrupt_common_entry.append((mnemonic, operands))
        is_xstate_instruction = (
            mnemonic.startswith("f")
            or mnemonic in STANDALONE_VECTOR
            or VECTOR_REGISTER.search(operands) is not None
        )
        if is_xstate_instruction and expected_xstate_instruction(
            current_symbol, mnemonic, operands
        ):
            assert current_symbol is not None
            observed_xstate[current_symbol].append(
                (mnemonic, normalized_operands(operands))
            )
        elif is_xstate_instruction:
            rejected.append(line.strip())

    if rejected:
        print("unexpected floating-point, vector, or xstate instructions found:")
        print("\n".join(rejected[:40]))
        return 1


    for symbol, expected in EXPECTED_XSTATE_INSTRUCTIONS.items():
        normalized_expected = [
            (mnemonic, normalized_operands(operands))
            for mnemonic, operands in expected
        ]
        if observed_xstate[symbol] != normalized_expected:
            print(f"{symbol} does not contain its exact audited xstate sequence")
            return 1

    if not nm_trigger_sets_ts(nm_trigger_instructions):
        print("scheduler_test_trigger_nm does not set CR0.TS exactly")
        return 1

    entry_error = interrupt_entry_error(interrupt_common_entry)
    if entry_error is not None:
        print(entry_error)
        return 1

    print("interrupt return has one exact branch-free RSP-to-iretq epilogue")
    print("only exact audited XSAVE and scheduler XMM probe instructions exist")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
