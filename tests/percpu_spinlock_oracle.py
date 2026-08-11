#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Differential oracle for the bounded per-CPU and spinlock state cores."""

from __future__ import annotations

import argparse
import random
import subprocess
import sys
from dataclasses import dataclass

U32_MAX = (1 << 32) - 1
CPU_LIMIT = 16
LOCK_LIMIT = 32
HELD_LIMIT = 8
CLASS_LIMIT = 9

P_OK = 0
P_POISONED = 4
P_INVALID_CPU = 5
P_INVALID_APIC = 6
P_ALREADY_ONLINE = 7
P_OFFLINE = 8
P_DUPLICATE_APIC = 9
P_BOOT_CPU = 10
P_STALE = 12
P_CORRUPTED = 13

S_OK = 0
S_POISONED = 4
S_INVALID_CPU = 5
S_CPU_OFFLINE = 6
S_INVALID_LOCK = 7
S_INVALID_ORDER = 8
S_ALREADY_REGISTERED = 9
S_NOT_REGISTERED = 10
S_BUSY = 11
S_CPU_BUSY = 12
S_RECURSIVE = 13
S_WRONG_OWNER = 14
S_ORDER_INVERSION = 15
S_NESTING_OVERFLOW = 16
S_INTERRUPTS_ENABLED = 17
S_STALE_TOKEN = 18


@dataclass
class Cpu:
    apic_id: int = U32_MAX
    generation: int = 0
    online: bool = False
    boot: bool = False


@dataclass
class Lock:
    generation: int = 0
    owner: int = U32_MAX
    order_class: int = 0
    rank: int = 0
    registered: bool = False
    locked: bool = False


@dataclass(frozen=True)
class Token:
    cpu: int
    lock: int
    generation: int
    saved_if: bool


class Model:
    def __init__(self) -> None:
        self.reset()

    def reset(self) -> None:
        self.cpus = [Cpu() for _ in range(CPU_LIMIT)]
        self.cpus[0] = Cpu(0, 1, True, True)
        self.cpu_next_generation = 2
        self.locks = [Lock() for _ in range(LOCK_LIMIT)]
        self.stacks: list[list[Token]] = [[] for _ in range(CPU_LIMIT)]
        self.lock_next_generation = 1
        self.poisoned = False

    def poison(self, status: int) -> int:
        self.poisoned = True
        return status

    def spin_gate(self) -> int:
        return S_POISONED if self.poisoned else S_OK

    def online(self, cpu: int, apic: int) -> int:
        if cpu >= CPU_LIMIT:
            return P_INVALID_CPU
        if apic == U32_MAX:
            return P_INVALID_APIC
        if self.cpus[cpu].online:
            return P_ALREADY_ONLINE
        if any(item.online and item.apic_id == apic for item in self.cpus):
            return P_DUPLICATE_APIC
        slot = self.cpus[cpu]
        slot.apic_id = apic
        slot.generation = self.cpu_next_generation
        slot.online = True
        self.cpu_next_generation += 1
        return P_OK

    def can_offline(self, cpu: int) -> int:
        gate = self.spin_gate()
        if gate != S_OK:
            return gate
        if cpu >= CPU_LIMIT:
            return S_INVALID_CPU
        if not self.cpus[cpu].online:
            return S_CPU_OFFLINE
        return S_CPU_BUSY if self.stacks[cpu] else S_OK

    def offline(self, cpu: int) -> tuple[int, int]:
        lock_status = self.can_offline(cpu)
        if lock_status != S_OK:
            return lock_status, P_CORRUPTED
        slot = self.cpus[cpu]
        if slot.boot:
            return S_OK, P_BOOT_CPU
        slot.apic_id = U32_MAX
        slot.online = False
        return S_OK, P_OK

    def lookup(self, apic: int) -> tuple[int, int]:
        if apic == U32_MAX:
            return P_INVALID_APIC, U32_MAX
        for index, cpu in enumerate(self.cpus):
            if cpu.online and cpu.apic_id == apic:
                return P_OK, index
        return P_OFFLINE, U32_MAX

    def handle(self, cpu: int) -> tuple[int, int, int]:
        if cpu >= CPU_LIMIT:
            return P_INVALID_CPU, U32_MAX, 0
        slot = self.cpus[cpu]
        if not slot.online:
            return P_OFFLINE, U32_MAX, 0
        return P_OK, cpu, slot.generation

    def resolve(self, cpu: int, generation: int) -> tuple[int, int]:
        if cpu >= CPU_LIMIT:
            return P_INVALID_CPU, U32_MAX
        slot = self.cpus[cpu]
        if not slot.online or generation == 0 or slot.generation != generation:
            return P_STALE, U32_MAX
        return P_OK, cpu

    def register(self, lock: int, order_class: int, rank: int) -> int:
        gate = self.spin_gate()
        if gate != S_OK:
            return gate
        if lock >= LOCK_LIMIT:
            return S_INVALID_LOCK
        if not 1 <= order_class < CLASS_LIMIT:
            return S_INVALID_ORDER
        slot = self.locks[lock]
        if slot.registered:
            return S_ALREADY_REGISTERED
        slot.registered = True
        slot.order_class = order_class
        slot.rank = rank & 0xFF
        return S_OK

    @staticmethod
    def before(outer: Lock, inner: Lock) -> bool:
        return (outer.order_class, outer.rank) < (
            inner.order_class,
            inner.rank,
        )

    def acquire(
        self, lock_index: int, cpu: int, interrupts_enabled: bool
    ) -> tuple[int, Token]:
        invalid = Token(U32_MAX, LOCK_LIMIT, 0, False)
        gate = self.spin_gate()
        if gate != S_OK:
            return gate, invalid
        if cpu >= CPU_LIMIT:
            return S_INVALID_CPU, invalid
        if not self.cpus[cpu].online:
            return S_CPU_OFFLINE, invalid
        if lock_index >= LOCK_LIMIT:
            return S_INVALID_LOCK, invalid
        lock = self.locks[lock_index]
        if not lock.registered:
            return S_NOT_REGISTERED, invalid
        if lock.locked:
            if lock.owner == cpu:
                return self.poison(S_RECURSIVE), invalid
            return S_BUSY, invalid
        stack = self.stacks[cpu]
        if len(stack) >= HELD_LIMIT:
            return self.poison(S_NESTING_OVERFLOW), invalid
        if stack and interrupts_enabled:
            return self.poison(S_INTERRUPTS_ENABLED), invalid
        if stack and not self.before(self.locks[stack[-1].lock], lock):
            return self.poison(S_ORDER_INVERSION), invalid
        generation = self.lock_next_generation
        self.lock_next_generation += 1
        lock.generation = generation
        lock.owner = cpu
        lock.locked = True
        token = Token(cpu, lock_index, generation, interrupts_enabled)
        stack.append(token)
        return S_OK, token

    def release(self, token: Token, interrupts_enabled: bool) -> tuple[int, bool]:
        gate = self.spin_gate()
        if gate != S_OK:
            return gate, False
        if interrupts_enabled:
            return self.poison(S_INTERRUPTS_ENABLED), False
        if token.cpu >= CPU_LIMIT:
            return self.poison(S_WRONG_OWNER), False
        if not self.cpus[token.cpu].online:
            return self.poison(S_CPU_OFFLINE), False
        if token.lock >= LOCK_LIMIT:
            return self.poison(S_INVALID_LOCK), False
        lock = self.locks[token.lock]
        if not lock.registered:
            return self.poison(S_NOT_REGISTERED), False
        if not lock.locked or lock.generation != token.generation or token.generation == 0:
            return self.poison(S_STALE_TOKEN), False
        stack = self.stacks[token.cpu]
        if lock.owner != token.cpu or not stack:
            return self.poison(S_WRONG_OWNER), False
        if stack[-1].lock != token.lock:
            return self.poison(S_ORDER_INVERSION), False
        if stack[-1].saved_if != token.saved_if:
            return self.poison(S_STALE_TOKEN), False
        stack.pop()
        lock.owner = U32_MAX
        lock.locked = False
        return S_OK, token.saved_if

    def snapshot(self, lock_index: int) -> tuple[int, int, int, int, int, int, int]:
        gate = self.spin_gate()
        if gate != S_OK:
            return gate, U32_MAX, 0, CLASS_LIMIT, 0, 0, 0
        if lock_index >= LOCK_LIMIT:
            return S_INVALID_LOCK, U32_MAX, 0, CLASS_LIMIT, 0, 0, 0
        lock = self.locks[lock_index]
        return (
            S_OK,
            lock.owner,
            lock.generation,
            lock.order_class,
            lock.rank,
            int(lock.registered),
            int(lock.locked),
        )


def line(prefix: str, *values: int) -> str:
    return " ".join((prefix, *(str(value) for value in values)))


def build_case(rng: random.Random, model: Model, tokens: list[Token]) -> tuple[str, str]:
    if model.poisoned and rng.random() < 0.72:
        model.reset()
        tokens.clear()
        return "C", "C 0 0"

    choice = rng.randrange(100)
    if choice < 4:
        model.reset()
        tokens.clear()
        return "C", "C 0 0"
    if choice < 9:
        return "V", line("V", P_OK, model.spin_gate())
    if choice < 18:
        cpu = rng.randrange(0, CPU_LIMIT + 4)
        apic = U32_MAX if rng.random() < 0.05 else rng.randrange(0, 24)
        return f"P {cpu} {apic}", line("P", model.online(cpu, apic))
    if choice < 25:
        cpu = rng.randrange(0, CPU_LIMIT + 3)
        lock_status, cpu_status = model.offline(cpu)
        return f"F {cpu}", line("F", lock_status, cpu_status)
    if choice < 31:
        if rng.random() < 0.65:
            apic = rng.choice(model.cpus).apic_id
        else:
            apic = rng.randrange(0, 28)
        status, cpu = model.lookup(apic)
        return f"L {apic}", line("L", status, cpu)
    if choice < 37:
        cpu = rng.randrange(0, CPU_LIMIT + 4)
        status, out_cpu, generation = model.handle(cpu)
        return f"H {cpu}", line("H", status, out_cpu, generation)
    if choice < 43:
        cpu = rng.randrange(0, CPU_LIMIT + 4)
        if cpu < CPU_LIMIT and rng.random() < 0.6:
            generation = model.cpus[cpu].generation
        else:
            generation = rng.randrange(0, model.cpu_next_generation + 3)
        status, out_cpu = model.resolve(cpu, generation)
        return f"E {cpu} {generation}", line("E", status, out_cpu)
    if choice < 53:
        lock_index = rng.randrange(0, LOCK_LIMIT + 5)
        order_class = rng.randrange(0, CLASS_LIMIT + 2)
        rank = rng.randrange(0, 256)
        status = model.register(lock_index, order_class, rank)
        return (
            f"R {lock_index} {order_class} {rank}",
            line("R", status),
        )
    if choice < 74:
        lock_index = rng.randrange(0, LOCK_LIMIT + 4)
        cpu = rng.randrange(0, CPU_LIMIT + 3)
        enabled = bool(rng.getrandbits(1))
        status, token = model.acquire(lock_index, cpu, enabled)
        if status == S_OK:
            tokens.append(token)
        return (
            f"A {lock_index} {cpu} {int(enabled)}",
            line(
                "A",
                status,
                token.cpu,
                token.lock,
                token.generation,
                int(token.saved_if),
            ),
        )
    if choice < 87:
        if tokens and rng.random() < 0.72:
            token = rng.choice(tokens)
            if rng.random() < 0.18:
                field = rng.randrange(4)
                token = Token(
                    token.cpu + (1 if field == 0 else 0),
                    token.lock + (1 if field == 1 else 0),
                    token.generation + (1 if field == 2 else 0),
                    (not token.saved_if) if field == 3 else token.saved_if,
                )
        else:
            token = Token(
                rng.randrange(0, CPU_LIMIT + 3),
                rng.randrange(0, LOCK_LIMIT + 3),
                rng.randrange(0, model.lock_next_generation + 3),
                bool(rng.getrandbits(1)),
            )
        interrupts_enabled = rng.random() < 0.08
        status, restore = model.release(token, interrupts_enabled)
        if status == S_OK:
            try:
                tokens.remove(token)
            except ValueError:
                pass
        return (
            f"U {token.cpu} {token.lock} {token.generation} "
            f"{int(token.saved_if)} {int(interrupts_enabled)}",
            line("U", status, int(restore)),
        )
    if choice < 93:
        cpu = rng.randrange(0, CPU_LIMIT + 3)
        return f"O {cpu}", line("O", model.can_offline(cpu))

    lock_index = rng.randrange(0, LOCK_LIMIT + 4)
    return f"S {lock_index}", line("S", *model.snapshot(lock_index))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("runner")
    parser.add_argument("--cases", type=int, default=250_000)
    parser.add_argument("--seed", type=int, default=0x5A17C0DE)
    args = parser.parse_args()
    if args.cases <= 0:
        parser.error("--cases must be positive")

    rng = random.Random(args.seed)
    model = Model()
    tokens: list[Token] = []
    commands: list[str] = []
    expected: list[str] = []
    for _ in range(args.cases):
        command, result = build_case(rng, model, tokens)
        commands.append(command)
        expected.append(result)

    completed = subprocess.run(
        [args.runner],
        input="\n".join(commands) + "\n",
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != 0:
        print(completed.stderr, file=sys.stderr, end="")
        print(f"runner exited with status {completed.returncode}", file=sys.stderr)
        return 1
    actual = completed.stdout.splitlines()
    if len(actual) != len(expected):
        print(
            f"output length mismatch: expected {len(expected)}, got {len(actual)}",
            file=sys.stderr,
        )
        return 1
    for index, (wanted, observed) in enumerate(zip(expected, actual)):
        if wanted != observed:
            print(f"mismatch at operation {index}", file=sys.stderr)
            print(f"command:  {commands[index]}", file=sys.stderr)
            print(f"expected: {wanted}", file=sys.stderr)
            print(f"actual:   {observed}", file=sys.stderr)
            return 1

    print(
        f"per-CPU/spinlock oracle passed {args.cases} operations "
        f"with seed {args.seed}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
