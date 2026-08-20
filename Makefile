SHELL := /bin/sh

BUILD_DIR := build
ISO_ROOT := $(BUILD_DIR)/iso-root
KERNEL := $(BUILD_DIR)/openseneri.elf
ISO := $(BUILD_DIR)/openseneri.iso
SERIAL_LOG := $(BUILD_DIR)/serial.log
TEST_BUILD_DIR := $(BUILD_DIR)/tests
TEST_SCENARIOS := normal breakpoint invalid-opcode page-fault ist pit unexpected \
	double-fault apic ioapic ioapic-level retired apic-timer tsc pm-timer \
	pit-retired timers paging heap pci pci-ecam threads thread-guard framebuffer \
	screen keyboard shell surface write-combining
TEST_TARGETS := $(addprefix qemu-test-,$(TEST_SCENARIOS))

CC := gcc
LD := ld
NM := nm
OBJDUMP := objdump
RUSTC := rustc
PYTHON := python3

# The one target Rust is built for. It matches the C flags exactly - no MMX, no
# SSE, soft float, no red zone - which is why the two halves can share a stack.
RUST_TARGET := x86_64-unknown-none
RUST_LIB := $(BUILD_DIR)/libopenseneri.a
RUST_SOURCES := $(wildcard src/rust/*.rs)
LOGO_SOURCE := assets/openseneri-logo.png
LOGO_BLOB := $(BUILD_DIR)/logo.srl
LOGO_MAX_DIMENSION := 256
FONT_SOURCE := tools/font8x16.txt
FONT_BLOB := $(BUILD_DIR)/font.snf

CPPFLAGS := -Iinclude
COMMON_FLAGS := -m64 -g -ffreestanding -fno-pie -fno-stack-protector
CFLAGS := $(COMMON_FLAGS) -std=c11 -O2 -mno-red-zone -mno-mmx -mno-sse \
	-mno-sse2 -msoft-float -fno-tree-vectorize -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -Wall -Wextra -Werror -Wpedantic -Wshadow -Wundef \
	-Wstrict-prototypes -Wmissing-prototypes
ASFLAGS := $(COMMON_FLAGS) -Wa,--fatal-warnings
# --orphan-handling=error is what keeps the two languages honest. A section
# neither linker.ld names nor discards is otherwise placed wherever ld prefers,
# which is how a Rust static library silently opened a gap between data and bss
# the first time one was linked in. Now an unnamed section is a link error.
LDFLAGS := -nostdlib -z max-page-size=0x1000 -z noexecstack --fatal-warnings \
	--orphan-handling=error --build-id=none -T linker.ld \
	-Map=$(BUILD_DIR)/openseneri.map

C_SOURCES := $(wildcard src/kernel/*.c)
C_OBJECTS := $(patsubst src/kernel/%.c,$(BUILD_DIR)/%.o,$(C_SOURCES))
ASM_SOURCES := $(wildcard src/arch/x86_64/*.S)
ASM_OBJECTS := $(patsubst src/arch/x86_64/%.S,$(BUILD_DIR)/arch_%.o,$(ASM_SOURCES))
OBJECTS := $(ASM_OBJECTS) $(C_OBJECTS)

# Warnings are errors on both sides of the language boundary, and Rust is held
# to the stricter rule that an unsafe operation inside an unsafe function still
# needs its own unsafe block naming why it is sound.
RUSTFLAGS := --edition 2024 --target $(RUST_TARGET) --crate-type staticlib \
	--crate-name openseneri -C panic=abort -C opt-level=2 \
	-C relocation-model=static -D warnings
DEPENDENCIES := $(C_OBJECTS:.o=.d)

# The qemu-test-% scenarios are deliberately absent from .PHONY. GNU Make skips
# implicit and pattern rule search for a phony target, so declaring them phony
# makes every scenario resolve to "nothing to be done" and pass without booting.
# They never create a file of their own name, so they rerun regardless.
.PHONY: all clean hooks iso kernel lint qemu-tests run smoke toolchain verify

all: kernel

kernel: $(KERNEL)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/arch_%.o: src/arch/x86_64/%.S | $(BUILD_DIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: src/kernel/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

# Regenerated only when the logo itself changes. The result is a build
# artifact and is deliberately not committed; src/rust/abi.rs includes it.
$(LOGO_BLOB): $(LOGO_SOURCE) tools/make-logo-asset.py | $(BUILD_DIR)
	$(PYTHON) tools/make-logo-asset.py $(LOGO_SOURCE) $(LOGO_MAX_DIMENSION) $@

# Regenerated only when the glyph art changes. Also a build artifact; the
# committed source is the ASCII art in $(FONT_SOURCE), so a clone needs nothing
# but Python to build the kernel.
$(FONT_BLOB): $(FONT_SOURCE) tools/make-font-asset.py | $(BUILD_DIR)
	$(PYTHON) tools/make-font-asset.py $(FONT_SOURCE) $@

$(RUST_LIB): $(RUST_SOURCES) $(LOGO_BLOB) $(FONT_BLOB) | $(BUILD_DIR)
	OPENSENERI_LOGO_BLOB='$(CURDIR)/$(LOGO_BLOB)' \
	OPENSENERI_FONT_BLOB='$(CURDIR)/$(FONT_BLOB)' \
		$(RUSTC) $(RUSTFLAGS) -o $@ src/rust/lib.rs

$(KERNEL): $(OBJECTS) $(RUST_LIB) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS) $(RUST_LIB)

toolchain:
	@for tool in gcc ld grub-file readelf nm objdump rustc python3; do \
		command -v $$tool >/dev/null 2>&1 || { echo "missing tool: $$tool"; exit 1; }; \
	done
	@version=$$($(RUSTC) --version | awk '{ print $$2 }'); \
		echo "$$version" | awk -F'[.-]' \
			'{ exit !($$1 > 1 || ($$1 == 1 && $$2 >= 85)) }' || \
		{ echo "rustc 1.85.0 or newer is required (found $$version)"; exit 1; }
	@$(RUSTC) --print target-list | grep -Fxq '$(RUST_TARGET)' || \
		{ echo 'rustc does not know $(RUST_TARGET)'; exit 1; }
	@libdir=$$($(RUSTC) --target $(RUST_TARGET) --print target-libdir 2>/dev/null) || \
		{ echo 'run: rustup target add $(RUST_TARGET)'; exit 1; }; \
		set -- "$$libdir"/libcore-*.rlib; \
		test -f "$$1" || \
		{ echo 'run: rustup target add $(RUST_TARGET)'; exit 1; }

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
	@if readelf -W -r $(KERNEL) | grep -Eq 'R_X86_64_'; then \
		echo 'kernel contains unresolved relocation records'; \
		readelf -W -r $(KERNEL); exit 1; \
	fi
	@test "$$($(NM) $(KERNEL) | grep -Ec ' [tT] interrupt_vector_[0-9]+$$')" -eq 256
	@$(OBJDUMP) -d $(KERNEL) | grep -Fq 'iretq'
	@$(OBJDUMP) -d $(KERNEL) | grep -Fq 'ltr'
	@$(OBJDUMP) -d $(KERNEL) | grep -Fq 'lidt'
	# This inspects the ELF file, and for a long time it was the only thing
	# behind OpenSeneri's W^X claim - while the kernel ran on boot.S's huge pages
	# with no NX bit enabled at all. It is kept because it catches a bad link
	# before anything boots, but the guarantee now rests on paging.c walking
	# the installed tables at runtime; see docs/VIRTUAL_MEMORY.md.
	@if readelf -W -l $(KERNEL) | grep -Eq 'LOAD[[:space:]].*RWE'; then \
		echo "kernel contains an RWX load segment"; exit 1; \
	fi
	@$(OBJDUMP) -d $(KERNEL) | grep -Fq 'invlpg'
	@$(NM) $(KERNEL) | grep -Eq ' [ABDRTt] __text_start$$'
	@$(NM) $(KERNEL) | grep -Eq ' [ABDRTt] __rodata_start$$'
	@$(NM) $(KERNEL) | grep -Eq ' [ABDRTt] __data_start$$'
	# The Rust half has to actually be in the image, and has to have been
	# linked as ordinary code rather than as something with its own runtime.
	@$(NM) $(KERNEL) | grep -Eq ' T seneri_logo_decode$$'
	@$(NM) $(KERNEL) | grep -Eq ' T seneri_logo_self_test$$'
	@$(NM) $(KERNEL) | grep -Eq ' T seneri_font_glyph$$'
	@$(NM) $(KERNEL) | grep -Eq ' T seneri_font_self_test$$'

$(ISO): $(KERNEL) grub/grub.cfg
	mkdir -p $(ISO_ROOT)/boot/grub
	cp $(KERNEL) $(ISO_ROOT)/boot/openseneri.elf
	cp grub/grub.cfg $(ISO_ROOT)/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(ISO_ROOT)

iso: $(ISO)

$(TEST_BUILD_DIR)/%/openseneri.iso: $(KERNEL) Makefile
	rm -rf $(TEST_BUILD_DIR)/$*
	mkdir -p $(TEST_BUILD_DIR)/$*/iso-root/boot/grub
	cp $(KERNEL) $(TEST_BUILD_DIR)/$*/iso-root/boot/openseneri.elf
	printf '%s\n' 'set default=0' 'set timeout=0' '' \
		'menuentry "OpenSeneri test" {' \
		'    multiboot2 /boot/openseneri.elf openseneri.test=$*' \
		'    boot' '}' >$(TEST_BUILD_DIR)/$*/iso-root/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(TEST_BUILD_DIR)/$*/iso-root

qemu-test-%: $(TEST_BUILD_DIR)/%/openseneri.iso
	@for tool in qemu-system-x86_64 timeout grep; do \
		command -v $$tool >/dev/null 2>&1 || { echo "missing tool: $$tool"; exit 1; }; \
	done
	# 0x22, which is status 69, remains assigned to the ioapic-level scenario;
	# the later scenarios start at 0x23 so every exit value stays stable.
	@case '$*' in \
		normal) expected=33 ;; \
		breakpoint) expected=35 ;; \
		invalid-opcode) expected=37 ;; \
		page-fault) expected=39 ;; \
		ist) expected=41 ;; \
		pit) expected=43 ;; \
		unexpected) expected=45 ;; \
		double-fault) expected=47 ;; \
		apic) expected=49 ;; \
		ioapic) expected=51 ;; \
		retired) expected=53 ;; \
		apic-timer) expected=55 ;; \
		tsc) expected=57 ;; \
		pm-timer) expected=59 ;; \
		pit-retired) expected=61 ;; \
		timers) expected=63 ;; \
		paging) expected=65 ;; \
		heap) expected=67 ;; \
		ioapic-level) expected=69 ;; \
		pci) expected=71 ;; \
		pci-ecam) expected=73 ;; \
		threads) expected=75 ;; \
		thread-guard) expected=77 ;; \
		framebuffer) expected=79 ;; \
		screen) expected=81 ;; \
		keyboard) expected=83 ;; \
		shell) expected=85 ;; \
		surface) expected=87 ;; \
		write-combining) expected=89 ;; \
		*) echo 'unknown QEMU scenario: $*'; exit 1 ;; \
	esac; \
		# Only pci-ecam departs from the default machine. i440fx publishes no \
		# MCFG, so every other scenario - including pci - proves the path that \
		# has nothing but the I/O ports. q35 is the only machine here with a \
		# PCI Express host bridge, and the root port is what gives the \
		# enumeration a second bus to find. Both PCI scenarios name their \
		# network device explicitly instead of relying on QEMU defaults. \
		case '$*' in \
			pci) hardware='-device e1000e' ;; \
			pci-ecam) \
				hardware='-machine q35 -device pcie-root-port,id=rp0,chassis=1 -device e1000e,bus=rp0 -device e1000e' ;; \
		*) hardware='' ;; \
	esac; \
	log='$(TEST_BUILD_DIR)/$*/serial.log'; \
	rm -f "$$log"; \
	set +e; \
	timeout 15s qemu-system-x86_64 \
		-machine accel=tcg -m 128M -smp 1 $$hardware \
		-cdrom '$<' -display none -monitor none -serial stdio \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		-no-reboot >"$$log" 2>&1; result=$$?; \
	set -e; \
	begin_count=$$(grep -Fxc 'ST BEGIN $*' "$$log" || true); \
	pass_count=$$(grep -Fxc 'ST PASS $*' "$$log" || true); \
	if test $$result -ne $$expected -o "$$begin_count" -ne 1 -o "$$pass_count" -ne 1 || \
		grep -Fq 'ST FAIL' "$$log" || grep -Fq 'OpenSeneri PANIC' "$$log"; then \
		echo 'QEMU scenario $* failed: status='$$result' expected='$$expected; \
		cat "$$log"; \
		exit 1; \
	fi; \
	if test '$*' = normal && \
		{ ! grep -Fq 'OpenSeneri: ACPI root verified' "$$log" || \
		  ! grep -Fq 'OpenSeneri: ACPI MADT verified' "$$log" || \
		  ! grep -Fq 'OpenSeneri: ACPI topology verified' "$$log" || \
		  ! grep -Eq '^OpenSeneri: ACPI I/O APIC id [0-9]+ at 0x' "$$log" || \
		  ! grep -Fq 'OpenSeneri: local APIC online' "$$log" || \
		  ! grep -Fq 'OpenSeneri: local APIC legacy routing LINT0 ExtINT' "$$log" || \
		  ! grep -Eq '^OpenSeneri: local APIC EOI-broadcast suppression (supported|unsupported) active (yes|no)$$' "$$log" || \
		  ! grep -Fq 'OpenSeneri: I/O APIC online' "$$log" || \
		  ! grep -Eq '^OpenSeneri: I/O APIC id [0-9]+ version 0x[0-9A-F]+ entries [0-9]+ base GSI [0-9]+ directed EOI (yes|no)$$' "$$log" || \
		  ! grep -Fq 'OpenSeneri: I/O APIC delivered eight interrupts' "$$log" || \
		  ! grep -Fq 'OpenSeneri: legacy 8259 retired' "$$log" || \
		  ! grep -Fq 'OpenSeneri: timer survives legacy retirement' "$$log" || \
		  ! grep -Eq '^OpenSeneri: I/O APIC level route id [0-9]+ GSI [0-9]+ vector [0-9]+ active (high|low) acknowledgement (directed|broadcast)$$' "$$log" || \
		  ! grep -Eq '^OpenSeneri: I/O APIC level deliveries [0-9]+ remote IRR [0-9]+ directed EOI [0-9]+ in [0-9]+ ns$$' "$$log" || \
		  ! grep -Fq 'OpenSeneri: I/O APIC delivered eight level-triggered interrupts' "$$log" || \
		  ! grep -Fq 'OpenSeneri: level-triggered routing established' "$$log" || \
		  ! grep -Eq '^OpenSeneri: local APIC timer calibrated at [0-9]+ counts' "$$log" || \
		  ! grep -Fq 'OpenSeneri: local APIC timer delivered eight interrupts' "$$log" || \
		  ! grep -Eq '^OpenSeneri: TSC calibrated at [0-9]+ Hz' "$$log" || \
		  ! grep -Fq 'OpenSeneri: TSC reference established' "$$log" || \
		  ! grep -Fq 'OpenSeneri: ACPI FADT verified' "$$log" || \
		  ! grep -Fq 'OpenSeneri: ACPI MCFG absent' "$$log" || \
		  ! grep -Fq 'OpenSeneri: ACPI configuration windows verified' "$$log" || \
		  ! grep -Eq '^OpenSeneri: ACPI PM timer port 0x[0-9A-F]+ width (24|32) bits address (fixed|extended)$$' "$$log" || \
		  ! grep -Eq '^OpenSeneri: PM timer counted [0-9]+ ticks in [0-9]+ ns$$' "$$log" || \
		  ! grep -Fq 'OpenSeneri: PM timer independent reference established' "$$log" || \
		  ! grep -Eq '^OpenSeneri: clocks agree: PM [0-9]+ ns, APIC timer [0-9]+ ns, TSC [0-9]+ ns$$' "$$log" || \
		  ! grep -Fq 'OpenSeneri: PIT retired' "$$log" || \
		  ! grep -Fq 'OpenSeneri: clocks survive PIT retirement' "$$log" || \
		  ! grep -Fq 'OpenSeneri: monotonic clock on time-stamp counter' "$$log" || \
		  ! grep -Eq '^OpenSeneri: slept [0-9]+ ns for a [0-9]+ ns deadline$$' "$$log" || \
		  ! grep -Fq 'OpenSeneri: deadline timers online' "$$log" || \
		  ! grep -Fq 'OpenSeneri: monotonic time established' "$$log" || \
		  ! grep -Eq '^OpenSeneri: paging root 0x[0-9A-F]+ table frames [0-9]+ regions [0-9]+ NX yes write protect yes$$' "$$log" || \
		  ! grep -Eq '^OpenSeneri: paging leaves [0-9]+ writable [0-9]+ executable [0-9]+ both 0$$' "$$log" || \
		  ! grep -Fq 'OpenSeneri: kernel page tables installed' "$$log" || \
		  ! grep -Fq 'OpenSeneri: no writable executable mapping' "$$log" || \
		  ! grep -Eq '^OpenSeneri: IA32_PAT before 0x[0-9A-F]{16} after 0x[0-9A-F]{16} entry 1 write-combining$$' "$$log" || \
		  ! grep -Eq '^OpenSeneri: framebuffer memory type write-combining pages [1-9][0-9]*$$' "$$log" || \
		  ! grep -Fq 'OpenSeneri: write-combining established' "$$log" || \
		  ! grep -Fq 'OpenSeneri: virtual memory established' "$$log" || \
		  ! grep -Eq '^OpenSeneri: heap window 0x[0-9A-F]+ size [0-9]+ guards 0x[0-9A-F]+ 0x[0-9A-F]+$$' "$$log" || \
		  ! grep -Eq '^OpenSeneri: heap committed [0-9]+ bytes in [0-9]+ pages, live 3$$' "$$log" || \
		  ! grep -Fq 'OpenSeneri: kernel heap online' "$$log" || \
		  ! grep -Fq 'OpenSeneri: heap coalesced to one free block' "$$log" || \
		  ! grep -Fq 'OpenSeneri: kernel heap established' "$$log" || \
		  ! grep -Eq '^OpenSeneri: deadline table of [0-9]+ entries on the heap$$' "$$log" || \
		  ! grep -Eq '^OpenSeneri: PCI mechanism 1 online, no window mapped$$' "$$log" || \
		  ! grep -Eq '^OpenSeneri: PCI buses [1-9][0-9]* functions [1-9][0-9]* bridges [0-9]+$$' "$$log" || \
		  ! grep -Eq '^OpenSeneri: PCI 0:0\.0 vendor 0x[0-9A-F]+ device 0x[0-9A-F]+ class 0x0*6\.0x0* ' "$$log" || \
		  ! grep -Fq 'OpenSeneri: PCI configuration space enumerated' "$$log" || \
		  ! grep -Fq 'OpenSeneri: PCI enumeration established' "$$log" || \
		  ! grep -Eq '^OpenSeneri: threads online, 3 ready of [0-9]+ on 12 stack frames$$' "$$log" || \
		  ! grep -Fxq 'OpenSeneri: thread rotation 123123123123' "$$log" || \
		  ! grep -Eq '^OpenSeneri: threads switched [1-9][0-9]* times, 3 exited$$' "$$log" || \
		  ! grep -Fq 'OpenSeneri: kernel threads established' "$$log" || \
		  ! grep -Eq '^OpenSeneri: framebuffer [0-9]+x[0-9]+ at 0x[0-9A-F]+ pitch [0-9]+ RGB [0-9]+/[0-9]+/[0-9]+$$' "$$log" || \
		  ! grep -Fxq 'OpenSeneri: framebuffer verified 786432 pixels' "$$log" || \
		  ! grep -Fq 'OpenSeneri: framebuffer established' "$$log" || \
		  ! grep -Eq '^OpenSeneri: surface [0-9]+x[0-9]+ pitch [0-9]+ buffer [0-9]+ bytes$$' "$$log" || \
		  ! grep -Eq '^OpenSeneri: surface cycles full present [0-9]+ one-line update [0-9]+ scroll [0-9]+$$' "$$log" || \
		  ! grep -Eq '^OpenSeneri: surface split cycles full draw [0-9]+ push [0-9]+ one-line draw [0-9]+ push [0-9]+ scroll draw [0-9]+ push [0-9]+$$' "$$log" || \
		  ! grep -Eq '^OpenSeneri: surface sparse two-corner cycles total [0-9]+ draw [0-9]+ push [0-9]+ union [0-9]+$$' "$$log" || \
		  ! grep -Eq '^OpenSeneri: surface copied [0-9]+ full, [0-9]+ line, [0-9]+ scroll pixels$$' "$$log" || \
		  ! grep -Fq 'OpenSeneri: cached surface established' "$$log" || \
		  ! grep -Eq '^OpenSeneri: screen console [0-9]+x[0-9]+ cells of 8x16, font [0-9]+ bytes$$' "$$log" || \
		  ! grep -Eq '^OpenSeneri: screen console drew [0-9]+ characters and scrolled [0-9]+ times$$' "$$log" || \
		  ! grep -Fq 'OpenSeneri: screen console established' "$$log" || \
		  ! grep -Fq 'OpenSeneri: screen console passed' "$$log" || \
		  ! grep -Eq '^OpenSeneri: keyboard 8042 online, IRQ 1 routed, [0-9]+ interrupts for [0-9]+ events$$' "$$log" || \
		  ! grep -Fxq 'OpenSeneri: keyboard decoded "hiI" from injected scancodes' "$$log" || \
		  ! grep -Fq 'OpenSeneri: keyboard established' "$$log" || \
		  ! grep -Fq 'OpenSeneri: keyboard passed' "$$log" || \
		  ! grep -Fxq 'OpenSeneri: shell ran "echo hi" from 8 injected scancodes' "$$log" || \
		  ! grep -Fq 'OpenSeneri: shell output verified on screen' "$$log" || \
		  ! grep -Fq 'OpenSeneri: shell established' "$$log" || \
		  ! grep -Fq 'OpenSeneri: shell passed' "$$log" || \
		  ! grep -Fq 'OpenSeneri: never triple fault milestone passed' "$$log"; }; then \
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
			grep -Fq 'OpenSeneri DOUBLE FAULT - HALTED' "$$log" || diagnostics_ok=false ;; \
		paging) \
			grep -Fq '  vector=14 name=page fault' "$$log" && \
			grep -Fq '  cr2=0x0000000200000000' "$$log" && \
			grep -Fq '  page-fault bits: P=1 W=1 U=0 RSVD=0 I=0' "$$log" || \
				diagnostics_ok=false ;; \
		heap) \
			grep -Fq '  vector=14 name=page fault' "$$log" && \
			grep -Fq '  cr2=0x0000000401000000' "$$log" && \
			grep -Fq '  page-fault bits: P=0 W=1 U=0 RSVD=0 I=0' "$$log" || \
				diagnostics_ok=false ;; \
		ioapic-level) \
			grep -Eq '^ST INFO ioapic-level: [0-9]+ deliveries, remote IRR [0-9]+, directed EOI [0-9]+, mode (directed|broadcast), in [0-9]+ ns$$' "$$log" || \
				diagnostics_ok=false ;; \
		pci) \
			grep -Eq '^ST PCI ports functions [0-9]+ buses [0-9]+$$' "$$log" && \
			! grep -Fq 'OpenSeneri: ACPI MCFG at' "$$log" || \
				diagnostics_ok=false ;; \
		pci-ecam) \
			grep -Fq 'OpenSeneri: ACPI MCFG at' "$$log" && \
			grep -Eq '^ST PCI window agreed on [0-9]+ registers of [0-9]+ functions across [0-9]+ buses, [0-9]+ with MSI-X$$' "$$log" && \
			! grep -Eq '^ST PCI window agreed on [0-9]+ registers of 0 functions' "$$log" || \
				diagnostics_ok=false ;; \
		threads) \
			grep -Eq '^ST THREADS created [0-9]+ switches [0-9]+ exited [0-9]+$$' "$$log" || \
				diagnostics_ok=false ;; \
		framebuffer) \
			grep -Eq '^ST FRAMEBUFFER [0-9]+x[0-9]+ probes 16 pitch [0-9]+$$' "$$log" || \
				diagnostics_ok=false ;; \
		surface) \
			grep -Eq '^ST SURFACE full [0-9]+ line [0-9]+ clipped 4 overlap both damage 20$$' "$$log" || \
				diagnostics_ok=false ;; \
		write-combining) \
			grep -Eq '^ST WRITE-COMBINING PAT 0x[0-9A-F]{16} ENTRY 1 FRAMEBUFFER [1-9][0-9]* PAGES$$' "$$log" || \
				diagnostics_ok=false ;; \
		thread-guard) \
			grep -Fq 'ST THREAD guard 0x0000000800005000' "$$log" && \
			grep -Fq '  vector=14 name=page fault' "$$log" && \
			grep -Fq '  cr2=0x0000000800005000' "$$log" && \
			grep -Fq '  page-fault bits: P=0 W=1 U=0 RSVD=0 I=0' "$$log" || \
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
