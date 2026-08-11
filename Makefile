SHELL := /bin/sh

BUILD_DIR := build
ISO_ROOT := $(BUILD_DIR)/iso-root
KERNEL := $(BUILD_DIR)/zenith.elf
ISO := $(BUILD_DIR)/zenith.iso
SERIAL_LOG := $(BUILD_DIR)/serial.log
TEST_BUILD_DIR := $(BUILD_DIR)/tests
HOST_TEST_DIR := $(BUILD_DIR)/host-tests
HOST_HEAP_RUNNER := $(HOST_TEST_DIR)/heap-core-runner
HOST_SCHEDULER_RUNNER := $(HOST_TEST_DIR)/scheduler-core-runner
HOST_SCHEDULER_LIFECYCLE_RUNNER := \
	$(HOST_TEST_DIR)/scheduler-lifecycle-runner
HOST_PREEMPT_RUNNER := $(HOST_TEST_DIR)/preempt-core-runner
HOST_PREEMPT_RUNTIME_RUNNER := $(HOST_TEST_DIR)/preempt-runtime-runner
HOST_PHYSICAL_GUARD_RUNNER := $(HOST_TEST_DIR)/physical-memory-guard-runner
HOST_TIME_IRQSAVE_RUNNER := $(HOST_TEST_DIR)/time-irqsave-runner
HOST_VIRTUAL_MEMORY_IRQSAVE_RUNNER := \
	$(HOST_TEST_DIR)/virtual-memory-irqsave-runner
HOST_VIRTUAL_MEMORY_EPOCH_RUNNER := \
	$(HOST_TEST_DIR)/virtual-memory-epoch-runner
HOST_PERCPU_SPINLOCK_RUNNER := $(HOST_TEST_DIR)/percpu-spinlock-core-runner
HOST_PERCPU_IRQSAVE_RUNTIME_RUNNER := \
	$(HOST_TEST_DIR)/percpu-irqsave-runtime-runner
HOST_ADDRESS_SPACE_RUNNER := $(HOST_TEST_DIR)/address-space-core-runner
HOST_PROCESS_SYSCALL_RUNNER := $(HOST_TEST_DIR)/process-syscall-core-runner
HOST_XSTATE_RUNNER := $(HOST_TEST_DIR)/xstate-core-runner
HOST_XSTATE_RUNTIME_RUNNER := $(HOST_TEST_DIR)/xstate-runtime-runner
HOST_XSTATE_CORE_OBJECT := $(HOST_TEST_DIR)/xstate-core.o
HOST_SANITIZER_DIR := $(BUILD_DIR)/host-sanitizers
HOST_SANITIZED_HEAP_RUNNER := $(HOST_SANITIZER_DIR)/heap-core-runner
HOST_SANITIZED_SCHEDULER_RUNNER := $(HOST_SANITIZER_DIR)/scheduler-core-runner
HOST_SANITIZED_SCHEDULER_LIFECYCLE_RUNNER := \
	$(HOST_SANITIZER_DIR)/scheduler-lifecycle-runner
HOST_SANITIZED_PREEMPT_RUNNER := $(HOST_SANITIZER_DIR)/preempt-core-runner
HOST_SANITIZED_PREEMPT_RUNTIME_RUNNER := \
	$(HOST_SANITIZER_DIR)/preempt-runtime-runner
HOST_SANITIZED_PHYSICAL_GUARD_RUNNER := \
	$(HOST_SANITIZER_DIR)/physical-memory-guard-runner
HOST_SANITIZED_TIME_IRQSAVE_RUNNER := \
	$(HOST_SANITIZER_DIR)/time-irqsave-runner
HOST_SANITIZED_VIRTUAL_MEMORY_IRQSAVE_RUNNER := \
	$(HOST_SANITIZER_DIR)/virtual-memory-irqsave-runner
HOST_SANITIZED_VIRTUAL_MEMORY_EPOCH_RUNNER := \
	$(HOST_SANITIZER_DIR)/virtual-memory-epoch-runner
HOST_SANITIZED_PERCPU_SPINLOCK_RUNNER := \
	$(HOST_SANITIZER_DIR)/percpu-spinlock-core-runner
HOST_SANITIZED_PERCPU_IRQSAVE_RUNTIME_RUNNER := \
	$(HOST_SANITIZER_DIR)/percpu-irqsave-runtime-runner
HOST_SANITIZED_ADDRESS_SPACE_RUNNER := \
	$(HOST_SANITIZER_DIR)/address-space-core-runner
HOST_SANITIZED_PROCESS_SYSCALL_RUNNER := \
	$(HOST_SANITIZER_DIR)/process-syscall-core-runner
HOST_SANITIZED_XSTATE_RUNNER := $(HOST_SANITIZER_DIR)/xstate-core-runner
HOST_SANITIZED_XSTATE_RUNTIME_RUNNER := \
	$(HOST_SANITIZER_DIR)/xstate-runtime-runner
ANALYZER_BUILD_DIR := $(BUILD_DIR)/analyzer
STACK_USAGE_BUILD_DIR := $(BUILD_DIR)/stack-usage
QEMU_RAM ?= 128M
QEMU_CPU ?= max
QEMU_MINIMUM_RAM := 19M
QEMU_HEAP_OOM_RAM := 18M
TEST_SCENARIOS := normal breakpoint invalid-opcode page-fault write-protect nx \
	ist pit apic lapic-timer heap unexpected double-fault scheduler \
	scheduler-guard scheduler-nm
TEST_TARGETS := $(addprefix qemu-test-,$(TEST_SCENARIOS))

CC := gcc
LD := ld
NM := nm
OBJDUMP := objdump
HOST_CC ?= cc
HOST_ABSOLUTE_SYMBOL_FLAGS := -fno-pie -no-pie

CPPFLAGS := -Iinclude
COMMON_FLAGS := -m64 -g -ffreestanding -fno-pie -fno-stack-protector
CFLAGS := $(COMMON_FLAGS) -std=c11 -O2 -mno-red-zone -mno-mmx -mno-sse \
	-mno-sse2 -msoft-float -fno-builtin -fno-tree-vectorize \
	-fno-omit-frame-pointer \
	-fno-optimize-sibling-calls -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -Wall -Wextra -Werror -Wpedantic -Wshadow -Wundef \
	-Wstrict-prototypes -Wmissing-prototypes
ASFLAGS := $(COMMON_FLAGS) -Wa,--fatal-warnings
LDFLAGS := -nostdlib -z max-page-size=0x1000 -z noexecstack --fatal-warnings \
	--build-id=none -T linker.ld -Map $(BUILD_DIR)/zenith.map

C_SOURCES := $(wildcard src/kernel/*.c)
C_OBJECTS := $(patsubst src/kernel/%.c,$(BUILD_DIR)/%.o,$(C_SOURCES))
ANALYZER_OBJECTS := \
	$(patsubst src/kernel/%.c,$(ANALYZER_BUILD_DIR)/%.o,$(C_SOURCES))
STACK_USAGE_OBJECTS := \
	$(patsubst src/kernel/%.c,$(STACK_USAGE_BUILD_DIR)/%.o,$(C_SOURCES))
ASM_SOURCES := $(wildcard src/arch/x86_64/*.S)
ASM_OBJECTS := $(patsubst src/arch/x86_64/%.S,$(BUILD_DIR)/arch_%.o,$(ASM_SOURCES))
OBJECTS := $(ASM_OBJECTS) $(C_OBJECTS)
DEPENDENCIES := $(C_OBJECTS:.o=.d)

.PHONY: all analyze binary-inspection bootstrap clean hooks hooks-check \
	host-sanitizers host-tests iso kernel lint qemu-minimum-tests qemu-stress \
	qemu-test-heap-oom qemu-tests run smoke stack-usage toolchain verify \
	$(TEST_TARGETS)

all: kernel

kernel: $(KERNEL)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/arch_%.o: src/arch/x86_64/%.S | $(BUILD_DIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: src/kernel/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(KERNEL): $(OBJECTS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS)

toolchain:
	@for tool in gcc ld grub-file readelf nm objdump python3; do \
		command -v $$tool >/dev/null 2>&1 || { echo "missing tool: $$tool"; exit 1; }; \
	done

lint:
	@if git grep -nI -E '[[:blank:]]+$$' -- . ':!assets/*'; then \
		echo "trailing whitespace is forbidden"; exit 1; \
	fi

$(HOST_TEST_DIR):
	mkdir -p $@

$(HOST_HEAP_RUNNER): tests/heap_core_runner.c src/kernel/heap_core.c \
		src/kernel/heap_core.h include/zenith/heap.h | $(HOST_TEST_DIR)
	$(HOST_CC) -Iinclude -std=c11 -O2 -Wall -Wextra -Werror -Wpedantic \
		-Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes \
		tests/heap_core_runner.c src/kernel/heap_core.c -o $@

$(HOST_SCHEDULER_RUNNER): tests/scheduler_core_runner.c \
		src/kernel/scheduler_core.c src/kernel/scheduler_core.h \
		include/zenith/scheduler.h | $(HOST_TEST_DIR)
	$(HOST_CC) -Iinclude -Isrc/kernel -std=c11 -O2 -Wall -Wextra -Werror \
		-Wpedantic -Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes \
		tests/scheduler_core_runner.c src/kernel/scheduler_core.c -o $@

$(HOST_SCHEDULER_LIFECYCLE_RUNNER): tests/scheduler_lifecycle_runner.c \
		src/kernel/scheduler_core.c src/kernel/scheduler_core.h \
		include/zenith/scheduler.h | $(HOST_TEST_DIR)
	$(HOST_CC) -Iinclude -Isrc/kernel -std=c11 -O2 -Wall -Wextra -Werror \
		-Wpedantic -Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes \
		tests/scheduler_lifecycle_runner.c src/kernel/scheduler_core.c -o $@

$(HOST_PREEMPT_RUNNER): tests/preempt_core_runner.c \
		src/kernel/preempt_core.c src/kernel/preempt_core.h \
		include/zenith/preempt.h | $(HOST_TEST_DIR)
	$(HOST_CC) -Iinclude -Isrc/kernel -std=c11 -O2 -Wall -Wextra -Werror \
		-Wpedantic -Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes \
		tests/preempt_core_runner.c src/kernel/preempt_core.c -o $@

$(HOST_PREEMPT_RUNTIME_RUNNER): tests/preempt_runtime_runner.c \
		src/kernel/percpu.c src/kernel/percpu_core.c \
		src/kernel/preempt.c src/kernel/preempt_core.c \
		src/kernel/preempt_core.h include/zenith/cpu.h \
		include/zenith/interrupts.h include/zenith/percpu.h \
		include/zenith/preempt.h | $(HOST_TEST_DIR)
	$(HOST_CC) -Iinclude -Isrc/kernel -std=c11 -O2 -Wall -Wextra -Werror \
		-Wpedantic -Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes \
	tests/preempt_runtime_runner.c src/kernel/percpu.c \
		src/kernel/percpu_core.c src/kernel/preempt.c \
		src/kernel/preempt_core.c -o $@

$(HOST_PHYSICAL_GUARD_RUNNER): tests/physical_memory_guard_runner.c \
		tests/physical_memory_guard_symbols.S src/kernel/physical_memory.c \
		include/zenith/boot.h include/zenith/memory.h include/zenith/preempt.h \
		include/zenith/spinlock.h \
		include/zenith/test.h | $(HOST_TEST_DIR)
	$(HOST_CC) $(HOST_ABSOLUTE_SYMBOL_FLAGS) -Iinclude -std=c11 -O2 \
		-Wall -Wextra -Werror -Wpedantic \
		-Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes \
		tests/physical_memory_guard_runner.c \
		tests/physical_memory_guard_symbols.S src/kernel/physical_memory.c -o $@

$(HOST_TIME_IRQSAVE_RUNNER): tests/time_irqsave_runner.c src/kernel/time.c \
		include/zenith/apic.h include/zenith/cpu.h \
		include/zenith/interrupts.h include/zenith/preempt.h \
		include/zenith/scheduler.h \
		include/zenith/spinlock.h include/zenith/time.h | $(HOST_TEST_DIR)
	$(HOST_CC) -Iinclude -std=c11 -O2 -Wall -Wextra -Werror -Wpedantic \
		-Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes \
		tests/time_irqsave_runner.c src/kernel/time.c -o $@

$(HOST_VIRTUAL_MEMORY_IRQSAVE_RUNNER): \
		tests/virtual_memory_irqsave_runner.c src/kernel/virtual_memory.c \
		include/zenith/acpi.h include/zenith/cpu.h include/zenith/memory.h \
		include/zenith/preempt.h include/zenith/spinlock.h \
		include/zenith/test.h include/zenith/virtual_memory.h | \
		$(HOST_TEST_DIR)
	$(HOST_CC) -Iinclude -std=c11 -O2 -Wall -Wextra -Werror -Wpedantic \
		-Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes \
		tests/virtual_memory_irqsave_runner.c \
		src/kernel/virtual_memory.c -o $@

$(HOST_VIRTUAL_MEMORY_EPOCH_RUNNER): \
		tests/virtual_memory_irqsave_runner.c \
		tests/virtual_memory_epoch_symbols.S src/kernel/virtual_memory.c \
		include/zenith/acpi.h include/zenith/cpu.h include/zenith/memory.h \
		include/zenith/preempt.h include/zenith/spinlock.h \
		include/zenith/test.h include/zenith/virtual_memory.h | \
		$(HOST_TEST_DIR)
	$(HOST_CC) -DVM_EPOCH_RUNNER -Iinclude -std=c11 -O2 -Wall -Wextra \
		-Werror -Wpedantic -Wshadow -Wundef -Wstrict-prototypes \
		-Wmissing-prototypes tests/virtual_memory_irqsave_runner.c \
		tests/virtual_memory_epoch_symbols.S src/kernel/virtual_memory.c -o $@

$(HOST_PERCPU_SPINLOCK_RUNNER): tests/percpu_spinlock_core_runner.c \
		src/kernel/percpu_core.c src/kernel/percpu_core.h \
		src/kernel/spinlock_core.c src/kernel/spinlock_core.h \
		include/zenith/percpu.h include/zenith/spinlock.h | $(HOST_TEST_DIR)
	$(HOST_CC) -Iinclude -Isrc/kernel -std=c11 -O2 -Wall -Wextra -Werror \
		-Wpedantic -Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes \
		tests/percpu_spinlock_core_runner.c src/kernel/percpu_core.c \
		src/kernel/spinlock_core.c -o $@

$(HOST_PERCPU_IRQSAVE_RUNTIME_RUNNER): \
		tests/percpu_irqsave_runtime_runner.c src/kernel/percpu.c \
		src/kernel/percpu_core.c src/kernel/percpu_core.h \
		src/kernel/preempt.c src/kernel/preempt_core.c \
		src/kernel/preempt_core.h src/kernel/spinlock.c \
		src/kernel/spinlock_core.c src/kernel/spinlock_core.h \
		include/zenith/cpu.h include/zenith/interrupts.h \
		include/zenith/percpu.h include/zenith/preempt.h \
		include/zenith/spinlock.h | $(HOST_TEST_DIR)
	$(HOST_CC) -Iinclude -Isrc/kernel -std=c11 -O2 -Wall -Wextra -Werror \
		-Wpedantic -Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes \
		-pthread tests/percpu_irqsave_runtime_runner.c src/kernel/percpu.c \
		src/kernel/percpu_core.c src/kernel/preempt.c \
		src/kernel/preempt_core.c src/kernel/spinlock.c \
		src/kernel/spinlock_core.c -o $@

$(HOST_ADDRESS_SPACE_RUNNER): tests/address_space_core_runner.c \
		src/kernel/address_space_core.c src/kernel/address_space_core.h \
		include/zenith/address_space.h | $(HOST_TEST_DIR)
	$(HOST_CC) -Iinclude -Isrc/kernel -std=c11 -O2 -Wall -Wextra -Werror \
		-Wpedantic -Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes \
		tests/address_space_core_runner.c src/kernel/address_space_core.c -o $@

$(HOST_PROCESS_SYSCALL_RUNNER): tests/process_syscall_core_runner.c \
		src/kernel/process_core.c src/kernel/process_core.h \
		src/kernel/syscall_core.c src/kernel/syscall_core.h \
		src/kernel/usercopy_core.c src/kernel/usercopy_core.h \
		include/zenith/process.h include/zenith/syscall.h \
		include/zenith/usercopy.h | $(HOST_TEST_DIR)
	$(HOST_CC) -Iinclude -Isrc/kernel -std=c11 -O2 -Wall -Wextra -Werror \
		-Wpedantic -Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes \
		tests/process_syscall_core_runner.c src/kernel/process_core.c \
		src/kernel/syscall_core.c src/kernel/usercopy_core.c -o $@

$(HOST_XSTATE_CORE_OBJECT): src/kernel/xstate_core.c src/kernel/xstate_core.h \
		include/zenith/xstate.h | $(HOST_TEST_DIR)
	$(HOST_CC) -Iinclude -Isrc/kernel -std=c11 -O2 -Wall -Wextra -Werror \
		-Wpedantic -Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes \
		-mno-mmx -mno-sse -mno-sse2 -msoft-float -fno-tree-vectorize \
		-c src/kernel/xstate_core.c -o $@

$(HOST_XSTATE_RUNNER): tests/xstate_core_runner.c src/kernel/xstate_core.c \
		src/kernel/xstate_core.h include/zenith/xstate.h | $(HOST_TEST_DIR)
	$(HOST_CC) -Iinclude -Isrc/kernel -std=c11 -O2 -Wall -Wextra -Werror \
		-Wpedantic -Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes \
		tests/xstate_core_runner.c src/kernel/xstate_core.c -o $@

$(HOST_XSTATE_RUNTIME_RUNNER): tests/xstate_runtime_runner.c \
		src/kernel/xstate.c src/kernel/xstate_core.c \
		src/kernel/xstate_core.h src/kernel/xstate_runtime.h \
		include/zenith/cpu.h include/zenith/xstate.h | $(HOST_TEST_DIR)
	$(HOST_CC) -Iinclude -Isrc/kernel -std=c11 -O2 -Wall -Wextra -Werror \
		-Wpedantic -Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes \
		tests/xstate_runtime_runner.c src/kernel/xstate.c \
		src/kernel/xstate_core.c -o $@

host-tests: $(HOST_HEAP_RUNNER) $(HOST_SCHEDULER_RUNNER) \
		$(HOST_SCHEDULER_LIFECYCLE_RUNNER) \
		$(HOST_PREEMPT_RUNNER) $(HOST_PREEMPT_RUNTIME_RUNNER) \
		$(HOST_PHYSICAL_GUARD_RUNNER) $(HOST_TIME_IRQSAVE_RUNNER) \
		$(HOST_VIRTUAL_MEMORY_IRQSAVE_RUNNER) \
		$(HOST_VIRTUAL_MEMORY_EPOCH_RUNNER) \
		$(HOST_PERCPU_SPINLOCK_RUNNER) \
		$(HOST_PERCPU_IRQSAVE_RUNTIME_RUNNER) \
		$(HOST_ADDRESS_SPACE_RUNNER) $(HOST_PROCESS_SYSCALL_RUNNER) \
		$(HOST_XSTATE_RUNNER) $(HOST_XSTATE_RUNTIME_RUNNER) \
		$(HOST_XSTATE_CORE_OBJECT)
	python3 tests/heap_oracle.py $(HOST_HEAP_RUNNER) --cases 100000
	python3 tests/scheduler_oracle.py $(HOST_SCHEDULER_RUNNER) --cases 250000
	python3 tests/scheduler_lifecycle_oracle.py \
		$(HOST_SCHEDULER_LIFECYCLE_RUNNER) --cases-per-seed 250000
	python3 tests/preempt_oracle.py $(HOST_PREEMPT_RUNNER) --cases 250000
	$(HOST_PREEMPT_RUNTIME_RUNNER)
	@for scenario in pre-init initialize certificate epoch-allocate-overflow \
		epoch-lifetime epoch-release heap-batch allocate allocate-owned release \
		reserve inject-failure inject-oom stats exact-if; do \
		$(HOST_PHYSICAL_GUARD_RUNNER) $$scenario || exit 1; \
	done
	@for scenario in pre-init init-if deferred-scheduler exact-if bad-frame \
		poison pinned-wait; do \
		$(HOST_TIME_IRQSAVE_RUNNER) $$scenario || exit 1; \
	done
	@for scenario in exact-if if-clear poison; do \
		$(HOST_VIRTUAL_MEMORY_IRQSAVE_RUNNER) $$scenario || exit 1; \
	done
	@for scenario in epoch-map-max-2 epoch-map-max-1 epoch-map-max \
		epoch-uncertain-cleanup epoch-map-restore epoch-unmap-restore \
		heap-map-roundtrip; do \
		$(HOST_VIRTUAL_MEMORY_EPOCH_RUNNER) $$scenario || exit 1; \
	done
	python3 tests/percpu_spinlock_oracle.py $(HOST_PERCPU_SPINLOCK_RUNNER) \
		--cases 250000
	@for scenario in early-boot basic recursive order stale wrong-owner \
		tampered corruption overflow contention subsystem-order; do \
		$(HOST_PERCPU_IRQSAVE_RUNTIME_RUNNER) $$scenario || exit 1; \
	done
	python3 tests/address_space_oracle.py $(HOST_ADDRESS_SPACE_RUNNER) \
		--operations 250000
	python3 tests/process_syscall_oracle.py $(HOST_PROCESS_SYSCALL_RUNNER) \
		--operations 300000
	python3 tests/xstate_oracle.py $(HOST_XSTATE_RUNNER) --cases 250000
	$(HOST_XSTATE_RUNTIME_RUNNER)
	$(HOST_XSTATE_RUNTIME_RUNNER) unexpected-nm
	python3 tests/xstate_disassembly_check.py $(HOST_XSTATE_CORE_OBJECT)
	python3 tests/physical_bitmap_oracle.py

$(HOST_SANITIZER_DIR):
	mkdir -p $@

$(HOST_SANITIZED_HEAP_RUNNER): tests/heap_core_runner.c \
		src/kernel/heap_core.c src/kernel/heap_core.h include/zenith/heap.h | \
		$(HOST_SANITIZER_DIR)
	$(HOST_CC) -Iinclude -std=c11 -O1 -g -Wall -Wextra -Werror -Wpedantic \
		-Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		tests/heap_core_runner.c src/kernel/heap_core.c -o $@

$(HOST_SANITIZED_SCHEDULER_RUNNER): tests/scheduler_core_runner.c \
		src/kernel/scheduler_core.c src/kernel/scheduler_core.h \
		include/zenith/scheduler.h | $(HOST_SANITIZER_DIR)
	$(HOST_CC) -Iinclude -Isrc/kernel -std=c11 -O1 -g -Wall -Wextra -Werror \
		-Wpedantic -Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		tests/scheduler_core_runner.c src/kernel/scheduler_core.c -o $@

$(HOST_SANITIZED_SCHEDULER_LIFECYCLE_RUNNER): \
		tests/scheduler_lifecycle_runner.c src/kernel/scheduler_core.c \
		src/kernel/scheduler_core.h include/zenith/scheduler.h | \
		$(HOST_SANITIZER_DIR)
	$(HOST_CC) -Iinclude -Isrc/kernel -std=c11 -O1 -g -Wall -Wextra -Werror \
		-Wpedantic -Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		tests/scheduler_lifecycle_runner.c src/kernel/scheduler_core.c -o $@

$(HOST_SANITIZED_PREEMPT_RUNNER): tests/preempt_core_runner.c \
		src/kernel/preempt_core.c src/kernel/preempt_core.h \
		include/zenith/preempt.h | $(HOST_SANITIZER_DIR)
	$(HOST_CC) -Iinclude -Isrc/kernel -std=c11 -O1 -g -Wall -Wextra -Werror \
		-Wpedantic -Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		tests/preempt_core_runner.c src/kernel/preempt_core.c -o $@

$(HOST_SANITIZED_PREEMPT_RUNTIME_RUNNER): \
		tests/preempt_runtime_runner.c src/kernel/percpu.c \
		src/kernel/percpu_core.c src/kernel/preempt.c \
		src/kernel/preempt_core.c src/kernel/preempt_core.h \
		include/zenith/cpu.h include/zenith/interrupts.h \
		include/zenith/percpu.h include/zenith/preempt.h | \
		$(HOST_SANITIZER_DIR)
	$(HOST_CC) -Iinclude -Isrc/kernel -std=c11 -O1 -g -Wall -Wextra \
		-Werror -Wpedantic -Wshadow -Wundef -Wstrict-prototypes \
		-Wmissing-prototypes -fsanitize=address,undefined \
		-fno-omit-frame-pointer tests/preempt_runtime_runner.c \
		src/kernel/percpu.c src/kernel/percpu_core.c src/kernel/preempt.c \
		src/kernel/preempt_core.c -o $@

$(HOST_SANITIZED_PHYSICAL_GUARD_RUNNER): \
		tests/physical_memory_guard_runner.c \
		tests/physical_memory_guard_symbols.S src/kernel/physical_memory.c \
		include/zenith/boot.h include/zenith/memory.h include/zenith/preempt.h \
		include/zenith/spinlock.h \
		include/zenith/test.h | $(HOST_SANITIZER_DIR)
	$(HOST_CC) $(HOST_ABSOLUTE_SYMBOL_FLAGS) -Iinclude -std=c11 -O1 -g \
		-Wall -Wextra -Werror -Wpedantic \
		-Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		tests/physical_memory_guard_runner.c \
		tests/physical_memory_guard_symbols.S src/kernel/physical_memory.c -o $@

$(HOST_SANITIZED_TIME_IRQSAVE_RUNNER): tests/time_irqsave_runner.c \
		src/kernel/time.c include/zenith/apic.h include/zenith/cpu.h \
		include/zenith/interrupts.h include/zenith/preempt.h \
		include/zenith/scheduler.h \
		include/zenith/spinlock.h include/zenith/time.h | \
		$(HOST_SANITIZER_DIR)
	$(HOST_CC) -Iinclude -std=c11 -O1 -g -Wall -Wextra -Werror -Wpedantic \
		-Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		tests/time_irqsave_runner.c src/kernel/time.c -o $@

$(HOST_SANITIZED_VIRTUAL_MEMORY_IRQSAVE_RUNNER): \
		tests/virtual_memory_irqsave_runner.c src/kernel/virtual_memory.c \
		include/zenith/acpi.h include/zenith/cpu.h include/zenith/memory.h \
		include/zenith/preempt.h include/zenith/spinlock.h \
		include/zenith/test.h include/zenith/virtual_memory.h | \
		$(HOST_SANITIZER_DIR)
	$(HOST_CC) -Iinclude -std=c11 -O1 -g -Wall -Wextra -Werror -Wpedantic \
		-Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		tests/virtual_memory_irqsave_runner.c \
		src/kernel/virtual_memory.c -o $@

$(HOST_SANITIZED_VIRTUAL_MEMORY_EPOCH_RUNNER): \
		tests/virtual_memory_irqsave_runner.c \
		tests/virtual_memory_epoch_symbols.S src/kernel/virtual_memory.c \
		include/zenith/acpi.h include/zenith/cpu.h include/zenith/memory.h \
		include/zenith/preempt.h include/zenith/spinlock.h \
		include/zenith/test.h include/zenith/virtual_memory.h | \
		$(HOST_SANITIZER_DIR)
	$(HOST_CC) -DVM_EPOCH_RUNNER -Iinclude -std=c11 -O1 -g -Wall -Wextra \
		-Werror -Wpedantic -Wshadow -Wundef -Wstrict-prototypes \
		-Wmissing-prototypes -fsanitize=address,undefined \
		-fno-omit-frame-pointer tests/virtual_memory_irqsave_runner.c \
		tests/virtual_memory_epoch_symbols.S src/kernel/virtual_memory.c -o $@

$(HOST_SANITIZED_PERCPU_SPINLOCK_RUNNER): \
		tests/percpu_spinlock_core_runner.c src/kernel/percpu_core.c \
		src/kernel/percpu_core.h src/kernel/spinlock_core.c \
		src/kernel/spinlock_core.h include/zenith/percpu.h \
		include/zenith/spinlock.h | $(HOST_SANITIZER_DIR)
	$(HOST_CC) -Iinclude -Isrc/kernel -std=c11 -O1 -g -Wall -Wextra -Werror \
		-Wpedantic -Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		tests/percpu_spinlock_core_runner.c src/kernel/percpu_core.c \
		src/kernel/spinlock_core.c -o $@

$(HOST_SANITIZED_PERCPU_IRQSAVE_RUNTIME_RUNNER): \
		tests/percpu_irqsave_runtime_runner.c src/kernel/percpu.c \
		src/kernel/percpu_core.c src/kernel/percpu_core.h \
		src/kernel/preempt.c src/kernel/preempt_core.c \
		src/kernel/preempt_core.h src/kernel/spinlock.c \
		src/kernel/spinlock_core.c src/kernel/spinlock_core.h \
		include/zenith/cpu.h include/zenith/interrupts.h \
		include/zenith/percpu.h include/zenith/preempt.h \
		include/zenith/spinlock.h | $(HOST_SANITIZER_DIR)
	$(HOST_CC) -Iinclude -Isrc/kernel -std=c11 -O1 -g -Wall -Wextra -Werror \
		-Wpedantic -Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes \
		-fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
		tests/percpu_irqsave_runtime_runner.c src/kernel/percpu.c \
		src/kernel/percpu_core.c src/kernel/preempt.c \
		src/kernel/preempt_core.c src/kernel/spinlock.c \
		src/kernel/spinlock_core.c -o $@

$(HOST_SANITIZED_ADDRESS_SPACE_RUNNER): tests/address_space_core_runner.c \
		src/kernel/address_space_core.c src/kernel/address_space_core.h \
		include/zenith/address_space.h | $(HOST_SANITIZER_DIR)
	$(HOST_CC) -Iinclude -Isrc/kernel -std=c11 -O1 -g -Wall -Wextra -Werror \
		-Wpedantic -Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		tests/address_space_core_runner.c src/kernel/address_space_core.c -o $@

$(HOST_SANITIZED_PROCESS_SYSCALL_RUNNER): \
		tests/process_syscall_core_runner.c src/kernel/process_core.c \
		src/kernel/process_core.h src/kernel/syscall_core.c \
		src/kernel/syscall_core.h src/kernel/usercopy_core.c \
		src/kernel/usercopy_core.h include/zenith/process.h \
		include/zenith/syscall.h include/zenith/usercopy.h | \
		$(HOST_SANITIZER_DIR)
	$(HOST_CC) -Iinclude -Isrc/kernel -std=c11 -O1 -g -Wall -Wextra -Werror \
		-Wpedantic -Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		tests/process_syscall_core_runner.c src/kernel/process_core.c \
		src/kernel/syscall_core.c src/kernel/usercopy_core.c -o $@

$(HOST_SANITIZED_XSTATE_RUNNER): tests/xstate_core_runner.c \
		src/kernel/xstate_core.c src/kernel/xstate_core.h \
		include/zenith/xstate.h | $(HOST_SANITIZER_DIR)
	$(HOST_CC) -Iinclude -Isrc/kernel -std=c11 -O1 -g -Wall -Wextra -Werror \
		-Wpedantic -Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		tests/xstate_core_runner.c src/kernel/xstate_core.c -o $@

$(HOST_SANITIZED_XSTATE_RUNTIME_RUNNER): tests/xstate_runtime_runner.c \
		src/kernel/xstate.c src/kernel/xstate_core.c \
		src/kernel/xstate_core.h src/kernel/xstate_runtime.h \
		include/zenith/cpu.h include/zenith/xstate.h | $(HOST_SANITIZER_DIR)
	$(HOST_CC) -Iinclude -Isrc/kernel -std=c11 -O1 -g -Wall -Wextra -Werror \
		-Wpedantic -Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		tests/xstate_runtime_runner.c src/kernel/xstate.c \
		src/kernel/xstate_core.c -o $@

host-sanitizers: $(HOST_SANITIZED_HEAP_RUNNER) \
		$(HOST_SANITIZED_SCHEDULER_RUNNER) \
		$(HOST_SANITIZED_SCHEDULER_LIFECYCLE_RUNNER) \
		$(HOST_SANITIZED_PREEMPT_RUNNER) \
		$(HOST_SANITIZED_PREEMPT_RUNTIME_RUNNER) \
		$(HOST_SANITIZED_PHYSICAL_GUARD_RUNNER) \
		$(HOST_SANITIZED_TIME_IRQSAVE_RUNNER) \
		$(HOST_SANITIZED_VIRTUAL_MEMORY_IRQSAVE_RUNNER) \
		$(HOST_SANITIZED_VIRTUAL_MEMORY_EPOCH_RUNNER) \
		$(HOST_SANITIZED_PERCPU_SPINLOCK_RUNNER) \
		$(HOST_SANITIZED_PERCPU_IRQSAVE_RUNTIME_RUNNER) \
		$(HOST_SANITIZED_ADDRESS_SPACE_RUNNER) \
		$(HOST_SANITIZED_PROCESS_SYSCALL_RUNNER) \
		$(HOST_SANITIZED_XSTATE_RUNNER) \
		$(HOST_SANITIZED_XSTATE_RUNTIME_RUNNER)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_string_checks=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		python3 tests/heap_oracle.py $(HOST_SANITIZED_HEAP_RUNNER) \
			--cases 100000
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_string_checks=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		python3 tests/scheduler_oracle.py $(HOST_SANITIZED_SCHEDULER_RUNNER) \
			--cases 250000
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_string_checks=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		python3 tests/scheduler_lifecycle_oracle.py \
			$(HOST_SANITIZED_SCHEDULER_LIFECYCLE_RUNNER) \
			--cases-per-seed 250000
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_string_checks=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		python3 tests/preempt_oracle.py $(HOST_SANITIZED_PREEMPT_RUNNER) \
			--cases 250000
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_string_checks=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		$(HOST_SANITIZED_PREEMPT_RUNTIME_RUNNER)
	@for scenario in pre-init initialize certificate epoch-allocate-overflow \
		epoch-lifetime epoch-release heap-batch allocate allocate-owned release \
		reserve inject-failure inject-oom stats exact-if; do \
		ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_string_checks=1 \
		UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
			$(HOST_SANITIZED_PHYSICAL_GUARD_RUNNER) $$scenario || exit 1; \
	done
	@for scenario in pre-init init-if deferred-scheduler exact-if bad-frame \
		poison pinned-wait; do \
		ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_string_checks=1 \
		UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
			$(HOST_SANITIZED_TIME_IRQSAVE_RUNNER) $$scenario || exit 1; \
	done
	@for scenario in exact-if if-clear poison; do \
		ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_string_checks=1 \
		UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
			$(HOST_SANITIZED_VIRTUAL_MEMORY_IRQSAVE_RUNNER) $$scenario || \
			exit 1; \
	done
	@for scenario in epoch-map-max-2 epoch-map-max-1 epoch-map-max \
		epoch-uncertain-cleanup epoch-map-restore epoch-unmap-restore \
		heap-map-roundtrip; do \
		ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_string_checks=1 \
		UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
			$(HOST_SANITIZED_VIRTUAL_MEMORY_EPOCH_RUNNER) $$scenario || \
			exit 1; \
	done
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_string_checks=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		python3 tests/percpu_spinlock_oracle.py \
			$(HOST_SANITIZED_PERCPU_SPINLOCK_RUNNER) --cases 250000
	@for scenario in early-boot basic recursive order stale wrong-owner \
		tampered corruption overflow contention subsystem-order; do \
		ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_string_checks=1 \
		UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
			$(HOST_SANITIZED_PERCPU_IRQSAVE_RUNTIME_RUNNER) $$scenario || \
			exit 1; \
	done
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_string_checks=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		python3 tests/address_space_oracle.py \
			$(HOST_SANITIZED_ADDRESS_SPACE_RUNNER) --operations 250000
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_string_checks=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		python3 tests/process_syscall_oracle.py \
			$(HOST_SANITIZED_PROCESS_SYSCALL_RUNNER) --operations 300000
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_string_checks=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		python3 tests/xstate_oracle.py $(HOST_SANITIZED_XSTATE_RUNNER) \
			--cases 250000
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_string_checks=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		$(HOST_SANITIZED_XSTATE_RUNTIME_RUNNER)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_string_checks=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		$(HOST_SANITIZED_XSTATE_RUNTIME_RUNNER) unexpected-nm

$(ANALYZER_BUILD_DIR):
	mkdir -p $@

$(ANALYZER_BUILD_DIR)/%.o: src/kernel/%.c | $(ANALYZER_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fanalyzer -Wformat=2 -Wpointer-arith \
		-Wvla -Walloca -Wdate-time -Wduplicated-cond -Wduplicated-branches \
		-Wlogical-op -Wjump-misses-init -c $< -o $@

analyze: $(ANALYZER_OBJECTS)
	@echo "GCC static analysis and additional strict warnings passed"

$(STACK_USAGE_BUILD_DIR):
	mkdir -p $@

$(STACK_USAGE_BUILD_DIR)/%.o: src/kernel/%.c | $(STACK_USAGE_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fstack-usage -Wstack-usage=8192 \
		-c $< -o $@

stack-usage: $(STACK_USAGE_OBJECTS)
	python3 tests/stack_usage_check.py $(STACK_USAGE_BUILD_DIR) --limit 8192

verify: toolchain lint host-tests
	$(MAKE) clean
	$(MAKE) kernel
	grub-file --is-x86-multiboot2 $(KERNEL)
	readelf -h $(KERNEL) | grep -Eq 'Class:[[:space:]]+ELF64'
	readelf -h $(KERNEL) | grep -Eq 'Machine:[[:space:]]+Advanced Micro Devices X86-64'
	@test -z "$$($(NM) -u $(KERNEL))" || { $(NM) -u $(KERNEL); exit 1; }
	@test "$$($(NM) $(KERNEL) | grep -Ec ' [tT] interrupt_vector_[0-9]+$$')" -eq 256
	@$(OBJDUMP) -d $(KERNEL) | grep -Fq 'iretq'
	@$(OBJDUMP) -d $(KERNEL) | grep -Fq 'ltr'
	@$(OBJDUMP) -d $(KERNEL) | grep -Fq 'lidt'
	@if readelf -W -l $(KERNEL) | grep -Eq 'LOAD[[:space:]].*RWE'; then \
		echo "kernel contains an RWX load segment"; exit 1; \
	fi
	$(MAKE) binary-inspection

binary-inspection: $(KERNEL)
	python3 tests/elf_inspection.py $(KERNEL)
	python3 tests/disassembly_check.py $(OBJDUMP) $(KERNEL)
	@test "$$($(NM) $(KERNEL) | grep -Ec ' [tT] interrupt_trigger_reschedule$$')" \
		-eq 1
	@test "$$($(NM) $(KERNEL) | grep -Ec ' [tT] scheduler_test_timer_register_probe$$')" \
		-eq 1
	@test "$$($(NM) $(KERNEL) | grep -Ec ' [tT] scheduler_test_trigger_nm$$')" \
		-eq 1
	@test "$$($(NM) $(KERNEL) | grep -Ec ' [tT] scheduler_nm_fault_site$$')" \
		-eq 1
	@test "$$($(NM) $(KERNEL) | grep -Ec ' [tT] scheduler_task_first_entry$$')" \
		-eq 1
	@test "$$($(NM) $(KERNEL) | grep -Ec ' [tT] scheduler_task_return_trampoline$$')" \
		-eq 1
	@$(OBJDUMP) -d $(KERNEL) | grep -Fq 'invlpg'
	@$(OBJDUMP) -d $(KERNEL) | grep -Eq '[[:space:]]cli$$'
	@$(OBJDUMP) -d $(KERNEL) | grep -Eq '[[:space:]]sti$$'
	@runtime_symbols="$$( \
		$(NM) $(KERNEL) | awk '{print $$NF}' | grep -E \
		'^__((ashl|ashr|div|mod|mul|udiv|umod)(si|di|ti)3|(u?divmod)(si|di|ti)4|fix[a-zA-Z0-9_]*|float[a-zA-Z0-9_]*|gcc_[a-zA-Z0-9_]*|stack_chk[a-zA-Z0-9_]*|ubsan_[a-zA-Z0-9_]*|asan_[a-zA-Z0-9_]*)$$' \
		|| true \
	)"; \
	if test -n "$$runtime_symbols"; then \
		echo "kernel contains an unexpected compiler-runtime symbol"; \
		printf '%s\n' "$$runtime_symbols"; \
		exit 1; \
	fi

$(ISO): $(KERNEL) grub/grub.cfg
	mkdir -p $(ISO_ROOT)/boot/grub
	cp $(KERNEL) $(ISO_ROOT)/boot/zenith.elf
	cp grub/grub.cfg $(ISO_ROOT)/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(ISO_ROOT)

iso: $(ISO)

$(TEST_BUILD_DIR)/%/zenith.iso: $(KERNEL) Makefile
	rm -rf $(TEST_BUILD_DIR)/$*
	mkdir -p $(TEST_BUILD_DIR)/$*/iso-root/boot/grub
	cp $(KERNEL) $(TEST_BUILD_DIR)/$*/iso-root/boot/zenith.elf
	printf '%s\n' 'set default=0' 'set timeout=0' '' \
		'menuentry "Zenith OS test" {' \
		'    multiboot2 /boot/zenith.elf zenith.test=$*' \
		'    boot' '}' >$(TEST_BUILD_DIR)/$*/iso-root/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(TEST_BUILD_DIR)/$*/iso-root

$(TEST_TARGETS): qemu-test-%: $(TEST_BUILD_DIR)/%/zenith.iso
	@for tool in qemu-system-x86_64 timeout grep; do \
		command -v $$tool >/dev/null 2>&1 || { echo "missing tool: $$tool"; exit 1; }; \
	done
	@case '$*' in \
		normal) expected=33 ;; \
		breakpoint) expected=35 ;; \
		invalid-opcode) expected=37 ;; \
		page-fault) expected=39 ;; \
		write-protect) expected=49 ;; \
		nx) expected=51 ;; \
		ist) expected=41 ;; \
		pit) expected=43 ;; \
		apic) expected=53 ;; \
		lapic-timer) expected=55 ;; \
		heap) expected=57 ;; \
		scheduler) expected=59 ;; \
		scheduler-guard) expected=61 ;; \
		scheduler-nm) expected=63 ;; \
		unexpected) expected=45 ;; \
		double-fault) expected=47 ;; \
		*) echo 'unknown QEMU scenario: $*'; exit 1 ;; \
	esac; \
	log='$(TEST_BUILD_DIR)/$*/serial.log'; \
	rm -f "$$log"; \
	set +e; \
	timeout 15s qemu-system-x86_64 \
		-machine accel=tcg -cpu '$(QEMU_CPU)' -m '$(QEMU_RAM)' -smp 1 \
		-cdrom '$<' -display none -monitor none -serial stdio \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		-no-reboot >"$$log" 2>&1; result=$$?; \
	set -e; \
	begin_count=$$(grep -Fxc 'ZT BEGIN $*' "$$log" || true); \
	pass_count=$$(grep -Fxc 'ZT PASS $*' "$$log" || true); \
	all_begin_count=$$(grep -Ec '^ZT BEGIN ' "$$log" || true); \
	all_pass_count=$$(grep -Ec '^ZT PASS ' "$$log" || true); \
	if test $$result -ne $$expected -o "$$begin_count" -ne 1 -o "$$pass_count" -ne 1 || \
		test "$$all_begin_count" -ne 1 -o "$$all_pass_count" -ne 1 || \
		grep -Fq 'ZT FAIL' "$$log" || grep -Fq 'Zenith OS PANIC' "$$log"; then \
		echo 'QEMU scenario $* failed: status='$$result' expected='$$expected; \
		cat "$$log"; \
		exit 1; \
	fi; \
	if test '$*' = normal && \
		{ ! grep -Fqx 'Zenith OS: day one passed' "$$log" || \
		  ! grep -Fqx 'Zenith OS: memory foundation passed' "$$log" || \
		  ! grep -Fqx 'Zenith OS: ACPI root verified' "$$log" || \
		  ! grep -Fqx 'Zenith OS: ACPI MADT verified' "$$log" || \
		  ! grep -Fqx 'Zenith OS: ACPI MADT topology verified' "$$log" || \
		  ! grep -Fqx 'Zenith OS: virtual memory foundation passed' "$$log" || \
		  ! grep -Fqx 'Zenith OS: per-CPU irqsave foundation online' "$$log" || \
		  ! grep -Fqx 'Zenith OS: APIC interrupt routing verified' "$$log" || \
		  ! grep -Fqx 'Zenith OS: Local APIC timer and monotonic clock verified' "$$log" || \
		  ! grep -Fqx 'Zenith OS: bounded kernel heap verified' "$$log" || \
		  ! grep -Fqx 'Zenith OS: bounded cooperative scheduler verified' "$$log" || \
		  ! grep -Fqx 'Zenith OS: never triple fault milestone passed' "$$log"; }; then \
		echo 'normal scenario did not complete the integrated production path'; \
		cat "$$log"; \
		exit 1; \
	fi; \
	diagnostics_ok=true; \
	case '$*' in \
		invalid-opcode) \
			grep -Fq '  vector=6 name=invalid opcode' "$$log" || diagnostics_ok=false ;; \
		page-fault) \
			grep -Fq '  vector=14 name=page fault' "$$log" && \
			grep -Fq '  cr2=0x0000000100000000' "$$log" && \
			grep -Fq '  page-fault bits: P=0 W=0 U=0 RSVD=0 I=0' "$$log" || \
				diagnostics_ok=false ;; \
		write-protect) \
			grep -Fq '  vector=14 name=page fault' "$$log" && \
			grep -Fq '  page-fault bits: P=1 W=1 U=0 RSVD=0 I=0' "$$log" || \
				diagnostics_ok=false ;; \
		nx) \
			grep -Fq '  vector=14 name=page fault' "$$log" && \
			grep -Fq '  page-fault bits: P=1 W=0 U=0 RSVD=0 I=1' "$$log" || \
				diagnostics_ok=false ;; \
		unexpected) \
			grep -Fq '  vector=128 name=unexpected vector' "$$log" || diagnostics_ok=false ;; \
		double-fault) \
			grep -Fq 'Zenith OS DOUBLE FAULT - HALTED' "$$log" || diagnostics_ok=false ;; \
		apic) \
			grep -Fq 'Zenith OS: APIC interrupt routing verified' "$$log" && \
			grep -Fq 'Zenith OS: legacy PIC permanently masked' "$$log" || \
				diagnostics_ok=false ;; \
		lapic-timer) \
			grep -Fqx 'Zenith OS: Local APIC timer and monotonic clock verified' "$$log" && \
			grep -Fqx 'Zenith OS: APIC-routed PIT delivered eight interrupts' "$$log" || \
				diagnostics_ok=false ;; \
		heap) \
			grep -Fqx 'Zenith OS: bounded kernel heap verified' "$$log" && \
			grep -Fqx 'Zenith OS: APIC calibration retry verified' "$$log" && \
			grep -Fqx 'Zenith OS: Local APIC timer and monotonic clock verified' "$$log" && \
			grep -Fqx 'Zenith OS: legacy PIC permanently masked' "$$log" || \
				diagnostics_ok=false ;; \
		scheduler) \
			grep -Fqx 'Zenith OS: bounded cooperative scheduler verified' "$$log" && \
			grep -Fqx 'ZT PROOF scheduler-lifecycle' "$$log" && \
			grep -Fqx 'ZT PROOF scheduler-stack-diagnostics' "$$log" && \
			grep -Fqx 'ZT PROOF scheduler-eager-xstate' "$$log" && \
			grep -Fqx 'Zenith OS: bounded kernel heap verified' "$$log" && \
			grep -Fqx 'Zenith OS: Local APIC timer and monotonic clock verified' "$$log" && \
			grep -Fqx 'Zenith OS: legacy PIC permanently masked' "$$log" || \
				diagnostics_ok=false ;; \
		scheduler-guard) \
			grep -Fq '  vector=14 name=page fault' "$$log" && \
			grep -Fiq '  cr2=0xffffa00000000000' "$$log" && \
			grep -Fq '  page-fault bits: P=0 W=0 U=0 RSVD=0 I=0' "$$log" || \
				diagnostics_ok=false ;; \
		scheduler-nm) \
			grep -Fq '  vector=7 name=device not available' "$$log" && \
			grep -Fq '  xstate=unexpected device-not-available' "$$log" || \
				diagnostics_ok=false ;; \
	esac; \
	if test "$$diagnostics_ok" != true; then \
		echo 'QEMU scenario $* omitted its required diagnostic'; \
		cat "$$log"; \
		exit 1; \
	fi; \
	echo 'QEMU scenario $* passed'

qemu-tests: $(TEST_TARGETS)
	@echo "all deterministic QEMU scenarios passed"

qemu-test-heap-oom: $(TEST_BUILD_DIR)/heap/zenith.iso
	@for tool in qemu-system-x86_64 timeout grep; do \
		command -v $$tool >/dev/null 2>&1 || { echo "missing tool: $$tool"; exit 1; }; \
	done
	@log='$(TEST_BUILD_DIR)/heap/serial-oom.log'; \
	rm -f "$$log"; \
	set +e; \
	timeout 15s qemu-system-x86_64 \
		-machine accel=tcg -cpu '$(QEMU_CPU)' \
		-m '$(QEMU_HEAP_OOM_RAM)' -smp 1 \
		-cdrom '$<' -display none -monitor none -serial stdio \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		-no-reboot >"$$log" 2>&1; result=$$?; \
	set -e; \
	if test $$result -ne 255 || \
		test "$$(grep -Fxc 'ZT BEGIN heap' "$$log" || true)" -ne 1 || \
		test "$$(grep -Ec '^ZT PASS ' "$$log" || true)" -ne 0 || \
		test "$$(grep -Fxc \
			'ZT FAIL heap: largest valid heap request failed' "$$log" || true)" \
			-ne 1 || grep -Fq 'Zenith OS PANIC' "$$log"; then \
		echo '18 MiB heap OOM boundary failed: status='$$result' expected=255'; \
		cat "$$log"; \
		exit 1; \
	fi; \
	grep -F 'Zenith OS: allocatable frames:' "$$log"; \
	echo '18 MiB heap OOM boundary passed with expected deterministic failure'

qemu-minimum-tests:
	$(MAKE) QEMU_RAM=$(QEMU_MINIMUM_RAM) qemu-tests
	@grep -F 'Zenith OS: allocatable frames:' \
		'$(TEST_BUILD_DIR)/normal/serial.log'
	$(MAKE) qemu-test-heap-oom

qemu-stress: $(TEST_BUILD_DIR)/scheduler/zenith.iso \
		$(TEST_BUILD_DIR)/scheduler-guard/zenith.iso
	python3 tests/qemu_scheduler_stress.py \
		--qemu qemu-system-x86_64 --cpu '$(QEMU_CPU)' --ram $(QEMU_RAM) \
		--scheduler-iso $(TEST_BUILD_DIR)/scheduler/zenith.iso \
		--guard-iso $(TEST_BUILD_DIR)/scheduler-guard/zenith.iso \
		--scheduler-runs 20 --guard-runs 20 --batch-size 2 \
		--guest-timeout 30

smoke: qemu-test-normal
	@echo "strict boot smoke test passed"

run: iso
	qemu-system-x86_64 -cpu '$(QEMU_CPU)' -cdrom $(ISO) -serial stdio \
		-no-reboot -no-shutdown

hooks:
	git config --local core.hooksPath .githooks
	@echo "repository hooks enabled"

hooks-check:
	@test "$$(git config --local --get core.hooksPath)" = '.githooks' || { \
		echo 'repository hooks are not enabled; run make bootstrap'; exit 1; \
	}
	@for hook in pre-commit pre-push; do \
		test -x ".githooks/$$hook" || { \
			echo "repository hook is not executable: .githooks/$$hook"; exit 1; \
		}; \
		sh -n ".githooks/$$hook" || exit 1; \
	done
	@echo "repository hook configuration, modes, and syntax verified"

bootstrap: toolchain
	@for tool in clang ld.lld grub-mkrescue mformat qemu-system-x86_64 \
			timeout grep sh xorriso; do \
		command -v $$tool >/dev/null 2>&1 || { \
			echo "missing development tool: $$tool"; exit 1; \
		}; \
	done
	$(MAKE) hooks
	$(MAKE) hooks-check
	@echo "complete local development toolchain and hooks verified"

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPENDENCIES)
