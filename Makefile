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
HOST_SANITIZER_DIR := $(BUILD_DIR)/host-sanitizers
HOST_SANITIZED_HEAP_RUNNER := $(HOST_SANITIZER_DIR)/heap-core-runner
HOST_SANITIZED_SCHEDULER_RUNNER := $(HOST_SANITIZER_DIR)/scheduler-core-runner
ANALYZER_BUILD_DIR := $(BUILD_DIR)/analyzer
STACK_USAGE_BUILD_DIR := $(BUILD_DIR)/stack-usage
QEMU_RAM ?= 128M
QEMU_MINIMUM_RAM := 19M
QEMU_HEAP_OOM_RAM := 18M
TEST_SCENARIOS := normal breakpoint invalid-opcode page-fault write-protect nx \
	ist pit apic lapic-timer heap unexpected double-fault scheduler \
	scheduler-guard
TEST_TARGETS := $(addprefix qemu-test-,$(TEST_SCENARIOS))

CC := gcc
LD := ld
NM := nm
OBJDUMP := objdump
HOST_CC ?= cc

CPPFLAGS := -Iinclude
COMMON_FLAGS := -m64 -g -ffreestanding -fno-pie -fno-stack-protector
CFLAGS := $(COMMON_FLAGS) -std=c11 -O2 -mno-red-zone -mno-mmx -mno-sse \
	-mno-sse2 -msoft-float -fno-tree-vectorize -fno-asynchronous-unwind-tables \
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

.PHONY: all analyze binary-inspection clean hooks host-sanitizers host-tests \
	iso kernel lint qemu-minimum-tests qemu-stress qemu-test-heap-oom \
	qemu-tests run smoke stack-usage toolchain verify \
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

host-tests: $(HOST_HEAP_RUNNER) $(HOST_SCHEDULER_RUNNER)
	python3 tests/heap_oracle.py $(HOST_HEAP_RUNNER) --cases 100000
	python3 tests/scheduler_oracle.py $(HOST_SCHEDULER_RUNNER) --cases 250000

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

host-sanitizers: $(HOST_SANITIZED_HEAP_RUNNER) \
		$(HOST_SANITIZED_SCHEDULER_RUNNER)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_string_checks=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		python3 tests/heap_oracle.py $(HOST_SANITIZED_HEAP_RUNNER) \
			--cases 100000
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_string_checks=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		python3 tests/scheduler_oracle.py $(HOST_SANITIZED_SCHEDULER_RUNNER) \
			--cases 250000

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
	@test "$$($(NM) $(KERNEL) | grep -Ec ' [tT] scheduler_context_switch$$')" \
		-eq 1
	@test "$$($(NM) $(KERNEL) | grep -Ec ' [tT] scheduler_task_first_entry$$')" \
		-eq 1
	@test "$$($(NM) $(KERNEL) | grep -Ec ' [tT] scheduler_task_return_trampoline$$')" \
		-eq 1
	@$(OBJDUMP) -d $(KERNEL) | grep -Fq 'invlpg'
	@$(OBJDUMP) -d $(KERNEL) | grep -Eq '[[:space:]]cli$$'
	@$(OBJDUMP) -d $(KERNEL) | grep -Eq '[[:space:]]sti$$'
	@if $(NM) $(KERNEL) | grep -Eq \
		'__(ashl|ashr|div|mod|mul|udiv|umod|fix|float|gcc|stack_chk|ubsan|asan)'; \
	then \
		echo "kernel contains an unexpected compiler-runtime symbol"; \
		$(NM) $(KERNEL) | grep -E \
			'__(ashl|ashr|div|mod|mul|udiv|umod|fix|float|gcc|stack_chk|ubsan|asan)'; \
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
		unexpected) expected=45 ;; \
		double-fault) expected=47 ;; \
		*) echo 'unknown QEMU scenario: $*'; exit 1 ;; \
	esac; \
	log='$(TEST_BUILD_DIR)/$*/serial.log'; \
	rm -f "$$log"; \
	set +e; \
	timeout 15s qemu-system-x86_64 \
		-machine accel=tcg -m '$(QEMU_RAM)' -smp 1 \
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
		{ ! grep -Fqx 'Zenith OS: ACPI root verified' "$$log" || \
		  ! grep -Fqx 'Zenith OS: ACPI MADT verified' "$$log" || \
		  ! grep -Fqx 'Zenith OS: ACPI MADT topology verified' "$$log" || \
		  ! grep -Fqx 'Zenith OS: virtual memory foundation passed' "$$log" || \
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
			grep -Fqx 'Zenith OS: bounded kernel heap verified' "$$log" && \
			grep -Fqx 'Zenith OS: Local APIC timer and monotonic clock verified' "$$log" && \
			grep -Fqx 'Zenith OS: legacy PIC permanently masked' "$$log" || \
				diagnostics_ok=false ;; \
		scheduler-guard) \
			grep -Fq '  vector=14 name=page fault' "$$log" && \
			grep -Fq '  cr2=0xffffa00000000000' "$$log" && \
			grep -Fq '  page-fault bits: P=0 W=0 U=0 RSVD=0 I=0' "$$log" || \
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
		-machine accel=tcg -m '$(QEMU_HEAP_OOM_RAM)' -smp 1 \
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
		--qemu qemu-system-x86_64 --ram $(QEMU_RAM) \
		--scheduler-iso $(TEST_BUILD_DIR)/scheduler/zenith.iso \
		--guard-iso $(TEST_BUILD_DIR)/scheduler-guard/zenith.iso \
		--scheduler-runs 20 --guard-runs 8 --batch-size 4

smoke: qemu-test-normal
	@echo "strict boot smoke test passed"

run: iso
	qemu-system-x86_64 -cdrom $(ISO) -serial stdio -no-reboot -no-shutdown

hooks:
	git config core.hooksPath .githooks
	@echo "repository hooks enabled"

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPENDENCIES)
