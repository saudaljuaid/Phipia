SHELL := /bin/sh

BUILD_DIR := build
ISO_ROOT := $(BUILD_DIR)/iso-root
KERNEL := $(BUILD_DIR)/zenith.elf
ISO := $(BUILD_DIR)/zenith.iso
SERIAL_LOG := $(BUILD_DIR)/serial.log
TEST_BUILD_DIR := $(BUILD_DIR)/tests
TEST_SCENARIOS := normal breakpoint invalid-opcode page-fault ist pit unexpected double-fault
TEST_TARGETS := $(addprefix qemu-test-,$(TEST_SCENARIOS))

CC := gcc
LD := ld
NM := nm
OBJDUMP := objdump

CPPFLAGS := -Iinclude
COMMON_FLAGS := -m64 -g -ffreestanding -fno-pie -fno-stack-protector
CFLAGS := $(COMMON_FLAGS) -std=c11 -O2 -mno-red-zone -mno-mmx -mno-sse \
	-mno-sse2 -msoft-float -fno-tree-vectorize -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -Wall -Wextra -Werror -Wpedantic -Wshadow -Wundef \
	-Wstrict-prototypes -Wmissing-prototypes
ASFLAGS := $(COMMON_FLAGS) -Wa,--fatal-warnings
LDFLAGS := -nostdlib -z max-page-size=0x1000 -z noexecstack --fatal-warnings \
	--build-id=none -T linker.ld -Map=$(BUILD_DIR)/zenith.map

C_SOURCES := $(wildcard src/kernel/*.c)
C_OBJECTS := $(patsubst src/kernel/%.c,$(BUILD_DIR)/%.o,$(C_SOURCES))
ASM_SOURCES := $(wildcard src/arch/x86_64/*.S)
ASM_OBJECTS := $(patsubst src/arch/x86_64/%.S,$(BUILD_DIR)/arch_%.o,$(ASM_SOURCES))
OBJECTS := $(ASM_OBJECTS) $(C_OBJECTS)
DEPENDENCIES := $(C_OBJECTS:.o=.d)

.PHONY: all clean hooks iso kernel lint qemu-tests run smoke toolchain verify \
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
	@for tool in gcc ld grub-file readelf nm objdump; do \
		command -v $$tool >/dev/null 2>&1 || { echo "missing tool: $$tool"; exit 1; }; \
	done

lint:
	@if git grep -nI -E '[[:blank:]]+$$' -- . ':!assets/*'; then \
		echo "trailing whitespace is forbidden"; exit 1; \
	fi

verify: toolchain lint
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

qemu-test-%: $(TEST_BUILD_DIR)/%/zenith.iso
	@for tool in qemu-system-x86_64 timeout grep; do \
		command -v $$tool >/dev/null 2>&1 || { echo "missing tool: $$tool"; exit 1; }; \
	done
	@case '$*' in \
		normal) expected=33 ;; \
		breakpoint) expected=35 ;; \
		invalid-opcode) expected=37 ;; \
		page-fault) expected=39 ;; \
		ist) expected=41 ;; \
		pit) expected=43 ;; \
		unexpected) expected=45 ;; \
		double-fault) expected=47 ;; \
		*) echo 'unknown QEMU scenario: $*'; exit 1 ;; \
	esac; \
	log='$(TEST_BUILD_DIR)/$*/serial.log'; \
	rm -f "$$log"; \
	set +e; \
	timeout 15s qemu-system-x86_64 \
		-machine accel=tcg -m 128M -smp 1 \
		-cdrom '$<' -display none -monitor none -serial stdio \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		-no-reboot >"$$log" 2>&1; result=$$?; \
	set -e; \
	begin_count=$$(grep -Fxc 'ZT BEGIN $*' "$$log" || true); \
	pass_count=$$(grep -Fxc 'ZT PASS $*' "$$log" || true); \
	if test $$result -ne $$expected -o "$$begin_count" -ne 1 -o "$$pass_count" -ne 1 || \
		grep -Fq 'ZT FAIL' "$$log" || grep -Fq 'Zenith OS PANIC' "$$log"; then \
		echo 'QEMU scenario $* failed: status='$$result' expected='$$expected; \
		cat "$$log"; \
		exit 1; \
	fi; \
	if test '$*' = normal && \
		{ ! grep -Fq 'Zenith OS: ACPI root verified' "$$log" || \
		  ! grep -Fq 'Zenith OS: ACPI MADT verified' "$$log" || \
		  ! grep -Fq 'Zenith OS: never triple fault milestone passed' "$$log"; }; then \
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
		unexpected) \
			grep -Fq '  vector=128 name=unexpected vector' "$$log" || diagnostics_ok=false ;; \
		double-fault) \
			grep -Fq 'Zenith OS DOUBLE FAULT - HALTED' "$$log" || diagnostics_ok=false ;; \
	esac; \
	if test "$$diagnostics_ok" != true; then \
		echo 'QEMU scenario $* omitted its required diagnostic'; \
		cat "$$log"; \
		exit 1; \
	fi; \
	echo 'QEMU scenario $* passed'

qemu-tests: $(TEST_TARGETS)
	@echo "all deterministic QEMU scenarios passed"

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
