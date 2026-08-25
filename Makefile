SHELL := /bin/sh

BUILD_DIR := build
ISO_ROOT := $(BUILD_DIR)/iso-root
KERNEL := $(BUILD_DIR)/sapote.elf
ISO := $(BUILD_DIR)/sapote.iso
SERIAL_LOG := $(BUILD_DIR)/serial.log
TEST_BUILD_DIR := $(BUILD_DIR)/tests
TEST_SCENARIOS := normal breakpoint invalid-opcode page-fault ist pit unexpected \
	double-fault apic ioapic ioapic-level retired apic-timer tsc pm-timer \
	pit-retired timers paging heap pci pci-ecam threads thread-guard framebuffer \
	screen keyboard shell surface write-combining device-windows \
	boot-ledger first-light device-substrate xhci nvme filesystem process \
	linux-abi linux-abi-uname first-light-userland \
	first-light-userland-absent first-light-userland-interactive \
	first-light-userland-interactive-absent \
	fat32-system fat32-data fat32-nested fat32-growth fat32-random \
	fat32-truncate fat32-rename fat32-delete fat32-full fat32-corrupt \
	fat32-missing fat32-persistence fat32-cache fat32-immutable fat32-handles
TEST_TARGETS := $(addprefix qemu-test-,$(TEST_SCENARIOS))
EXPECTED_TEST_SCENARIO_COUNT := 58
EXPECTED_SHELL_ASSERTION_COUNT := 359

CC := gcc
LD := ld
NM := nm
OBJDUMP := objdump
RUSTC := rustc
PYTHON := python3
FFMPEG ?= ffmpeg
QEMU_ACCEL ?= tcg
GRUB_MKRESCUE ?= grub-mkrescue
GRUB_MODULE_DIR ?=
GRUB_MKRESCUE_FLAGS := $(if $(GRUB_MODULE_DIR),-d $(GRUB_MODULE_DIR),)

# The one target Rust is built for. It matches the C flags exactly - no MMX, no
# SSE, soft float, no red zone - which is why the two halves can share a stack.
RUST_TARGET := x86_64-unknown-none
RUST_LIB := $(BUILD_DIR)/libsapote.a
RUST_FAT16_TEST := $(BUILD_DIR)/fat16-tests
RUST_FAT32_TEST := $(BUILD_DIR)/fat32-tests
RUST_LINUX_FAT16_TEST := $(BUILD_DIR)/linux-fat16-tests
RUST_LINUX_ELF64_TEST := $(BUILD_DIR)/linux-elf64-tests
RUST_LINUX_UNAME_FAT16_TEST := $(BUILD_DIR)/linux-uname-fat16-tests
RUST_LINUX_UNAME_ELF64_TEST := $(BUILD_DIR)/linux-uname-elf64-tests
RUST_LINUX_CAT_FAT16_TEST := $(BUILD_DIR)/linux-cat-fat16-tests
RUST_LINUX_CAT_ELF64_TEST := $(BUILD_DIR)/linux-cat-elf64-tests
RUST_ELF64_TEST := $(BUILD_DIR)/elf64-tests
RUST_SOURCES := $(wildcard src/rust/*.rs)
LOGO_SOURCE := assets/sapote-logo.png
LOGO_BLOB := $(BUILD_DIR)/logo.srl
LOGO_MAX_DIMENSION := 280
STUDIO_ICON_SOURCE := assets/sapstudio-icon.png
STUDIO_ICON_BLOB := $(BUILD_DIR)/sapstudio-icon.srl
STUDIO_ICON_MAX_DIMENSION := 80
WALLPAPER_SOURCE := assets/sapote-first-environment-wallpaper.png
WALLPAPER_BLOB := $(BUILD_DIR)/wallpaper.spw
FONT_SOURCE := tools/font8x16.txt
FONT_BLOB := $(BUILD_DIR)/font.snf
UI_FONT_SOURCE := assets/fonts/spleen-8x16.bdf
UI_FONT_LICENSE := assets/fonts/Spleen-LICENSE
UI_FONT_BLOB := $(BUILD_DIR)/ui-font.suf
FIRST_LIGHT_IMAGE := assets/sapote-v2-first-light.png
FIRST_LIGHT_FOCUS_IMAGE := assets/sapote-v2-first-light-focus.png
FIRST_LIGHT_TERMINAL_IMAGE := assets/sapote-v2-first-light-terminal.png
FIRST_LIGHT_CAPTURE_DIR := $(BUILD_DIR)/first-light-captures
FIRST_LIGHT_BOOT_VIDEO := assets/sapote-first-light-boot-20s.mp4
FIRST_ENVIRONMENT_CAPTURE_DIR := $(BUILD_DIR)/first-environment-captures
FIRST_ENVIRONMENT_IMAGE := assets/sapote-first-environment.png
FIRST_ENVIRONMENT_DOCK_IMAGE := assets/sapote-first-environment-dock.png
FIRST_ENVIRONMENT_TERMINAL_IMAGE := \
	assets/sapote-first-environment-terminal.png
FIRST_ENVIRONMENT_FILES_IMAGE := assets/sapote-first-environment-files.png
FIRST_ENVIRONMENT_NOTES_IMAGE := assets/sapote-first-environment-notes.png
FIRST_ENVIRONMENT_STUDIO_IMAGE := assets/sapote-first-environment-studio.png
FIRST_ENVIRONMENT_VIDEO := assets/sapote-first-environment-20s.mp4
NVME_FIXTURE := $(TEST_BUILD_DIR)/nvme/nvme-fixture.raw
FILESYSTEM_FIXTURE := $(TEST_BUILD_DIR)/filesystem/fat16-fixture.raw
PROCESS_ELF := $(TEST_BUILD_DIR)/process/SAPOTE.BIN
PROCESS_FIXTURE := $(TEST_BUILD_DIR)/process/process-fixture.raw
BUSYBOX_OUTPUT_DIR := $(BUILD_DIR)/busybox-contract
BUSYBOX_WORK_DIR := $(BUILD_DIR)/busybox-work
BUSYBOX_BINARY := $(BUSYBOX_OUTPUT_DIR)/busybox
LINUX_ABI_FIXTURE := $(BUILD_DIR)/fixtures/linux-abi-fat16.raw
BUSYBOX_UNAME_OUTPUT_DIR := $(BUILD_DIR)/busybox-uname-contract
BUSYBOX_UNAME_WORK_DIR := $(BUILD_DIR)/busybox-uname-work
BUSYBOX_UNAME_BINARY := $(BUSYBOX_UNAME_OUTPUT_DIR)/busybox
LINUX_UNAME_FIXTURE := $(BUILD_DIR)/fixtures/linux-uname-fat16.raw
BUSYBOX_CAT_OUTPUT_DIR := $(BUILD_DIR)/busybox-cat-contract
BUSYBOX_CAT_WORK_DIR := $(BUILD_DIR)/busybox-cat-work
BUSYBOX_CAT_BINARY := $(BUSYBOX_CAT_OUTPUT_DIR)/busybox
FIRST_LIGHT_USERLAND_IMAGE := $(BUILD_DIR)/userspace/sapote-userland-fat16.raw
FIRST_LIGHT_USERLAND_NO_CAT_IMAGE := \
	$(BUILD_DIR)/userspace/sapote-userland-no-cat-fat16.raw
FAT32_SYSTEM_IMAGE := $(BUILD_DIR)/userspace/sapote-system-fat32.raw
FAT32_DATA_IMAGE := $(BUILD_DIR)/userspace/sapote-data-fat32.raw
FAT32_RUN_DATA_IMAGE := $(BUILD_DIR)/run-data-fat32.raw
FAT32_FULL_IMAGE := $(BUILD_DIR)/userspace/sapote-data-full-fat32.raw
FAT32_CORRUPT_IMAGE := $(BUILD_DIR)/userspace/sapote-data-corrupt-fat32.raw

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
	-Map=$(BUILD_DIR)/sapote.map

C_SOURCES := $(wildcard src/kernel/*.c)
C_OBJECTS := $(patsubst src/kernel/%.c,$(BUILD_DIR)/%.o,$(C_SOURCES))
ASM_SOURCES := $(wildcard src/arch/x86_64/*.S)
ASM_OBJECTS := $(patsubst src/arch/x86_64/%.S,$(BUILD_DIR)/arch_%.o,$(ASM_SOURCES))
OBJECTS := $(ASM_OBJECTS) $(C_OBJECTS)

# Warnings are errors on both sides of the language boundary, and Rust is held
# to the stricter rule that an unsafe operation inside an unsafe function still
# needs its own unsafe block naming why it is sound. The measured 5,136-byte
# FAT chain is the largest Rust aggregate; allowing 1,024 direct stores keeps
# its bounded copies and zeroing inline instead of introducing a GOT-backed
# compiler memory call into the fixed-address kernel. One codegen unit also
# keeps calls between Rust boundary functions direct; the two variable-sized
# font copies use explicit bounded byte operations instead of runtime calls.
RUSTFLAGS := --edition 2024 --target $(RUST_TARGET) --crate-type staticlib \
	--crate-name sapote -C panic=abort -C opt-level=2 \
	-C relocation-model=static -C codegen-units=1 \
	-C llvm-args=-max-store-memcpy=1024 \
	-C llvm-args=-max-store-memset=1024 -D warnings
DEPENDENCIES := $(C_OBJECTS:.o=.d)

# The qemu-test-% scenarios are deliberately absent from .PHONY. GNU Make skips
# implicit and pattern rule search for a phony target, so declaring them phony
# makes every scenario resolve to "nothing to be done" and pass without booting.
# They never create a file of their own name, so they rerun regardless.
.PHONY: all capture-boot-video capture-first-environment capture-first-light clean contract-counts contract-scenarios fat32-images hooks \
	iso kernel lint qemu-tests run screenshot-proof smoke toolchain verify

all: kernel

contract-counts:
	@printf '%s %s\n' '$(EXPECTED_TEST_SCENARIO_COUNT)' \
		'$(EXPECTED_SHELL_ASSERTION_COUNT)'

contract-scenarios:
	@printf '%s\n' $(TEST_SCENARIOS)

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

$(STUDIO_ICON_BLOB): $(STUDIO_ICON_SOURCE) tools/make-logo-asset.py | $(BUILD_DIR)
	$(PYTHON) tools/make-logo-asset.py $(STUDIO_ICON_SOURCE) \
		$(STUDIO_ICON_MAX_DIMENSION) $@

$(WALLPAPER_BLOB): $(WALLPAPER_SOURCE) tools/make-wallpaper-asset.py | $(BUILD_DIR)
	$(PYTHON) tools/make-wallpaper-asset.py $(WALLPAPER_SOURCE) $@

# Regenerated only when the glyph art changes. Also a build artifact; the
# committed source is the ASCII art in $(FONT_SOURCE), so a clone needs nothing
# but Python to build the kernel.
$(FONT_BLOB): $(FONT_SOURCE) tools/make-font-asset.py | $(BUILD_DIR)
	$(PYTHON) tools/make-font-asset.py $(FONT_SOURCE) $@

$(UI_FONT_BLOB): $(UI_FONT_SOURCE) tools/make-ui-font-asset.py | $(BUILD_DIR)
	$(PYTHON) tools/make-ui-font-asset.py $(UI_FONT_SOURCE) $@

$(RUST_LIB): $(RUST_SOURCES) $(LOGO_BLOB) $(STUDIO_ICON_BLOB) \
		$(WALLPAPER_BLOB) $(FONT_BLOB) $(UI_FONT_BLOB) | $(BUILD_DIR)
	SAPOTE_LOGO_BLOB='$(CURDIR)/$(LOGO_BLOB)' \
	SAPOTE_STUDIO_ICON_BLOB='$(CURDIR)/$(STUDIO_ICON_BLOB)' \
	SAPOTE_WALLPAPER_BLOB='$(CURDIR)/$(WALLPAPER_BLOB)' \
	SAPOTE_FONT_BLOB='$(CURDIR)/$(FONT_BLOB)' \
	SAPOTE_UI_FONT_BLOB='$(CURDIR)/$(UI_FONT_BLOB)' \
		$(RUSTC) $(RUSTFLAGS) -o $@ src/rust/lib.rs

$(BUSYBOX_BINARY): tools/build-busybox-proof.sh \
		tools/check-exercised-instructions.py \
		userspace/busybox/busybox.config \
		userspace/busybox/source/busybox-1.38.0.tar.bz2 \
		userspace/busybox/source/musl-1.2.6.tar.gz
	SAPOTE_BUSYBOX_BUILD_ONLY=1 bash tools/build-busybox-proof.sh \
		$(BUSYBOX_OUTPUT_DIR) $(BUSYBOX_WORK_DIR)

$(LINUX_ABI_FIXTURE): $(BUSYBOX_BINARY) tools/make-linux-abi-fixture.py
	mkdir -p $(dir $@)
	$(PYTHON) tools/make-linux-abi-fixture.py $(BUSYBOX_BINARY) $@

$(BUSYBOX_UNAME_BINARY): tools/build-busybox-uname-proof.sh \
		tools/check-exercised-instructions.py \
		userspace/busybox/busybox-uname.config \
		userspace/busybox/musl-vfprintf-scalar.h \
		userspace/busybox/source/busybox-1.38.0.tar.bz2 \
		userspace/busybox/source/musl-1.2.6.tar.gz
	SAPOTE_BUSYBOX_BUILD_ONLY=1 bash tools/build-busybox-uname-proof.sh \
		$(BUSYBOX_UNAME_OUTPUT_DIR) $(BUSYBOX_UNAME_WORK_DIR)

$(LINUX_UNAME_FIXTURE): $(BUSYBOX_UNAME_BINARY) \
		tools/make-linux-uname-fixture.py
	mkdir -p $(dir $@)
	$(PYTHON) tools/make-linux-uname-fixture.py $(BUSYBOX_UNAME_BINARY) $@

$(BUSYBOX_CAT_BINARY): tools/build-busybox-cat-proof.sh \
		tools/check-exercised-instructions.py \
		userspace/busybox/busybox-cat.config \
		userspace/busybox/source/busybox-1.38.0.tar.bz2 \
		userspace/busybox/source/musl-1.2.6.tar.gz
	SAPOTE_BUSYBOX_BUILD_ONLY=1 bash tools/build-busybox-cat-proof.sh \
		$(BUSYBOX_CAT_OUTPUT_DIR) $(BUSYBOX_CAT_WORK_DIR)

$(FIRST_LIGHT_USERLAND_IMAGE): $(BUSYBOX_BINARY) $(BUSYBOX_UNAME_BINARY) \
		$(BUSYBOX_CAT_BINARY) \
		tools/make-first-light-userland.py
	mkdir -p $(dir $@)
	$(PYTHON) tools/make-first-light-userland.py \
		$(BUSYBOX_BINARY) $(BUSYBOX_UNAME_BINARY) $(BUSYBOX_CAT_BINARY) $@

$(FIRST_LIGHT_USERLAND_NO_CAT_IMAGE): $(BUSYBOX_BINARY) \
		$(BUSYBOX_UNAME_BINARY) tools/make-first-light-userland.py
	mkdir -p $(dir $@)
	$(PYTHON) tools/make-first-light-userland.py \
		$(BUSYBOX_BINARY) $(BUSYBOX_UNAME_BINARY) --without-cat $@

$(FAT32_SYSTEM_IMAGE): $(BUSYBOX_BINARY) $(BUSYBOX_UNAME_BINARY) \
		$(BUSYBOX_CAT_BINARY) tools/fat32_image.py
	mkdir -p $(dir $@)
	$(PYTHON) tools/fat32_image.py format system $@ \
		--echo $(BUSYBOX_BINARY) --uname $(BUSYBOX_UNAME_BINARY) \
		--cat $(BUSYBOX_CAT_BINARY)

$(FAT32_DATA_IMAGE): tools/fat32_image.py
	mkdir -p $(dir $@)
	$(PYTHON) tools/fat32_image.py format data $@

$(FAT32_FULL_IMAGE): tools/fat32_image.py
	mkdir -p $(dir $@)
	$(PYTHON) tools/fat32_image.py format data $@ --full

$(FAT32_CORRUPT_IMAGE): $(FAT32_DATA_IMAGE) tools/fat32_image.py
	mkdir -p $(dir $@)
	$(PYTHON) tools/fat32_image.py malform fat-mismatch \
		$(FAT32_DATA_IMAGE) $@

fat32-images: $(FAT32_SYSTEM_IMAGE) $(FAT32_DATA_IMAGE)

$(KERNEL): $(OBJECTS) $(RUST_LIB) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS) $(RUST_LIB) || { \
		rm -f $@; \
		sed -n '/__got_start/,/__got_end/p' $(BUILD_DIR)/sapote.map; \
		sed 's/ASSERT(__got_end == __got_start,/ASSERT(1,/' \
			linker.ld >$(BUILD_DIR)/linker-got-diagnostic.ld; \
		$(LD) -nostdlib -z max-page-size=0x1000 -z noexecstack \
			--orphan-handling=error --build-id=none --emit-relocs \
			-T $(BUILD_DIR)/linker-got-diagnostic.ld \
			-o $(BUILD_DIR)/sapote-got-diagnostic.elf \
			$(OBJECTS) $(RUST_LIB) || true; \
		readelf -W -r $(BUILD_DIR)/sapote-got-diagnostic.elf \
			| grep 'GOT' || true; \
		$(OBJDUMP) -dr $(RUST_LIB) \
			| grep -B 8 -A 2 'R_X86_64_GOTPCREL' || true; \
		exit 1; \
	}

toolchain:
	@for tool in bash bzip2 gcc gzip ld grub-file readelf nm objdump rustc python3 sha256sum strings tar; do \
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
	@test "$$(sha256sum $(LOGO_SOURCE) | awk '{ print toupper($$1) }')" = \
		'16EC9B0CB3DCB098EB52EF6FB27A2EBF6BBB615B1A0F9EB0B67D21D1D1F77317'
	@test "$$(sha256sum assets/sapote-logo-source.jpeg | awk '{ print toupper($$1) }')" = \
		'0743231ED884F16A8D973A5E7C6D51F2EF97D2F1E386D59FF8A35024AC2EDE3C'
	@test "$$(sha256sum $(WALLPAPER_SOURCE) | awk '{ print toupper($$1) }')" = \
		'3984C670E9788B3E9858834FD1696D9213630A19F64D6691B7AF5D511CE099C4'
	@test "$$(sha256sum $(WALLPAPER_BLOB) | awk '{ print toupper($$1) }')" = \
		'3D09707C534D24D834210345CDB61CAA10E31F4C706FD389C2F1B6374AA486D0'
	@test '$(LOGO_MAX_DIMENSION)' -eq 280
	@test "$$(sha256sum $(STUDIO_ICON_SOURCE) | awk '{ print toupper($$1) }')" = \
		'C5D706B274132B5FCAF0BB016D0DA56DDD1DC54B417709364874AD1A58611EB5'
	@test "$$(sha256sum $(STUDIO_ICON_BLOB) | awk '{ print toupper($$1) }')" = \
		'0E30BA0BFD43EE19ECBEF903AE2BF05CAB4C0F1E9E28E0C9233907FF6F3BCDDD'
	@test '$(STUDIO_ICON_MAX_DIMENSION)' -eq 80
	@test "$$(sha256sum $(UI_FONT_SOURCE) | awk '{ print toupper($$1) }')" = \
		'4A3D97EE61A8C86A7525D8C723CB8A14081F395CD2FEB4227BA5E3BAF0629BAE'
	@test "$$(sha256sum $(UI_FONT_LICENSE) | awk '{ print toupper($$1) }')" = \
		'F33FE8679D5B2ABECC4F1313CE6C6BFA58262964DE5F7BCA146596A7318047AF'
	@test "$$(sha256sum $(UI_FONT_BLOB) | awk '{ print toupper($$1) }')" = \
		'D6AD364D9E4A932EB753B83C7EF866DDAF09DDFF8B66BC9669F844267A26CE74'
	$(PYTHON) tools/make-fat16-fixture.py $(FILESYSTEM_FIXTURE)
	@test "$$(sha256sum $(FILESYSTEM_FIXTURE) | awk '{ print toupper($$1) }')" = \
		'B8FE53B80AAC718B36B545CC7A741ADCA52DF3BFE0DEE580D2A179B49DEBA5AC'
	$(PYTHON) tools/make-elf64-fixture.py $(PROCESS_ELF)
	@test "$$(sha256sum $(PROCESS_ELF) | awk '{ print toupper($$1) }')" = \
		'C923A94F08DF64523D3DB701E4F9FC5FF5B51DFC21447E1DC57586D40D42B8A9'
	$(PYTHON) tools/make-process-fixture.py $(PROCESS_FIXTURE)
	@test "$$(sha256sum $(PROCESS_FIXTURE) | awk '{ print toupper($$1) }')" = \
		'5130D78A0FEB51EC410E5CC931A1E6485D96549A726E62BCE95F7D5C18FA2290'
	$(RUSTC) --edition 2024 --test -D warnings src/rust/fat16.rs \
		-o $(RUST_FAT16_TEST)
	$(RUST_FAT16_TEST)
	$(RUSTC) --edition 2024 --test -D warnings src/rust/fat32.rs \
		-o $(RUST_FAT32_TEST)
	$(RUST_FAT32_TEST)
	$(PYTHON) -u tools/fat32_host_test.py
	$(MAKE) $(LINUX_ABI_FIXTURE)
	@test "$$(sha256sum $(LINUX_ABI_FIXTURE) | awk '{ print toupper($$1) }')" = \
		'41513E5D6F4C33F898F887D4F40F37149A29B1AE13B5E8A600495C18A38C7A6F'
	SAPOTE_BUSYBOX_BINARY='$(CURDIR)/$(BUSYBOX_BINARY)' \
		$(RUSTC) --edition 2024 --test -D warnings \
		tools/linux-fat16-host-test.rs -o $(RUST_LINUX_FAT16_TEST)
	$(RUST_LINUX_FAT16_TEST)
	SAPOTE_BUSYBOX_BINARY='$(CURDIR)/$(BUSYBOX_BINARY)' \
		$(RUSTC) --edition 2024 --test -D warnings \
		tools/linux-elf64-host-test.rs -o $(RUST_LINUX_ELF64_TEST)
	$(RUST_LINUX_ELF64_TEST)
	$(MAKE) $(LINUX_UNAME_FIXTURE)
	@test "$$(sha256sum $(LINUX_UNAME_FIXTURE) | awk '{ print toupper($$1) }')" = \
		'48C3465E924D1D2B3C8AB659D2783CAC4AF57DFD83504606AD0DF8F64D7316E3'
	SAPOTE_UNAME_BUSYBOX_BINARY='$(CURDIR)/$(BUSYBOX_UNAME_BINARY)' \
		$(RUSTC) --edition 2024 --test -D warnings \
		tools/linux-uname-fat16-host-test.rs \
		-o $(RUST_LINUX_UNAME_FAT16_TEST)
	$(RUST_LINUX_UNAME_FAT16_TEST)
	SAPOTE_UNAME_BUSYBOX_BINARY='$(CURDIR)/$(BUSYBOX_UNAME_BINARY)' \
		$(RUSTC) --edition 2024 --test -D warnings \
		tools/linux-uname-elf64-host-test.rs \
		-o $(RUST_LINUX_UNAME_ELF64_TEST)
	$(RUST_LINUX_UNAME_ELF64_TEST)
	$(MAKE) $(BUSYBOX_CAT_BINARY)
	@test "$$(sha256sum $(BUSYBOX_CAT_BINARY) | awk '{ print toupper($$1) }')" = \
		'8191596A22778B575942895071A2E50CCEEE0F82F4D88B6D986584CE0914FC3E'
	SAPOTE_CAT_BUSYBOX_BINARY='$(CURDIR)/$(BUSYBOX_CAT_BINARY)' \
		$(RUSTC) --edition 2024 --test -D warnings \
		tools/linux-cat-fat16-host-test.rs \
		-o $(RUST_LINUX_CAT_FAT16_TEST)
	$(RUST_LINUX_CAT_FAT16_TEST)
	SAPOTE_CAT_BUSYBOX_BINARY='$(CURDIR)/$(BUSYBOX_CAT_BINARY)' \
		$(RUSTC) --edition 2024 --test -D warnings \
		tools/linux-cat-elf64-host-test.rs \
		-o $(RUST_LINUX_CAT_ELF64_TEST)
	$(RUST_LINUX_CAT_ELF64_TEST)
	$(MAKE) $(FIRST_LIGHT_USERLAND_IMAGE)
	@test "$$(sha256sum $(FIRST_LIGHT_USERLAND_IMAGE) | awk '{ print toupper($$1) }')" = \
		'F2115B909842ADACB8460287515E5145E36B34DE7E0B8C658E92D22DDFA7EBDB'
	$(MAKE) $(FIRST_LIGHT_USERLAND_NO_CAT_IMAGE)
	@test "$$(sha256sum $(FIRST_LIGHT_USERLAND_NO_CAT_IMAGE) | awk '{ print toupper($$1) }')" = \
		'12F7EB4B4EE2F39CA721623AFCC6D337964FB32D2F081893DF182101514211CE'
	$(MAKE) $(FAT32_SYSTEM_IMAGE) $(FAT32_DATA_IMAGE) \
		$(FAT32_FULL_IMAGE) $(FAT32_CORRUPT_IMAGE)
	@test "$$(sha256sum $(FAT32_SYSTEM_IMAGE) | awk '{ print toupper($$1) }')" = \
		'5C9463E3C62A7351E12F7B7832C8A97FF016A264F9C7F356A76201DF4E17317E'
	@test "$$(sha256sum $(FAT32_DATA_IMAGE) | awk '{ print toupper($$1) }')" = \
		'890CDCC20ACB0FC1C09CC5977C5276F22682073C91A5E1DF0AD85483CC375288'
	@test "$$(sha256sum $(FAT32_FULL_IMAGE) | awk '{ print toupper($$1) }')" = \
		'EEB712F939ED9A3EA8E446F890CF20D4B6CCD9CBBE3746C9BD4CBAD75301301F'
	@test "$$(sha256sum $(FAT32_CORRUPT_IMAGE) | awk '{ print toupper($$1) }')" = \
		'10960E8383BBAA919B354BB1FEF42AECCEA7EE588AAB2734B79149AAC17460EC'
	rm -rf $(BUILD_DIR)/fat32-reconstruction
	mkdir -p $(BUILD_DIR)/fat32-reconstruction
	$(PYTHON) tools/fat32_image.py format system \
		$(BUILD_DIR)/fat32-reconstruction/system.raw \
		--echo $(BUSYBOX_BINARY) --uname $(BUSYBOX_UNAME_BINARY) \
		--cat $(BUSYBOX_CAT_BINARY)
	$(PYTHON) tools/fat32_image.py format data \
		$(BUILD_DIR)/fat32-reconstruction/data.raw
	cmp $(FAT32_SYSTEM_IMAGE) $(BUILD_DIR)/fat32-reconstruction/system.raw
	cmp $(FAT32_DATA_IMAGE) $(BUILD_DIR)/fat32-reconstruction/data.raw
	$(PYTHON) tools/fat32_image.py verify system $(FAT32_SYSTEM_IMAGE) \
		--echo $(BUSYBOX_BINARY) --uname $(BUSYBOX_UNAME_BINARY) \
		--cat $(BUSYBOX_CAT_BINARY)
	$(PYTHON) tools/fat32_image.py verify data $(FAT32_DATA_IMAGE)
	$(PYTHON) tools/fat32_image.py verify data $(FAT32_FULL_IMAGE) --full
	$(RUSTC) --edition 2024 --test -D warnings src/rust/elf64.rs \
		-o $(RUST_ELF64_TEST)
	$(RUST_ELF64_TEST)
	@test "$(words $(TEST_SCENARIOS))" -eq \
		'$(EXPECTED_TEST_SCENARIO_COUNT)'
	@grep -Fq '#define SHELL_PROMPT "sap> "' src/kernel/shell.c
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
	# behind Sapote's W^X claim - while the kernel ran on boot.S's huge pages
	# with no NX bit enabled at all. It is kept because it catches a bad link
	# before anything boots, but the guarantee now rests on paging.c walking
	# the installed tables at runtime.
	@if readelf -W -l $(KERNEL) | grep -Eq 'LOAD[[:space:]].*RWE'; then \
		echo "kernel contains an RWX load segment"; exit 1; \
	fi
	@$(OBJDUMP) -d $(KERNEL) | grep -Fq 'invlpg'
	@forbidden="$$( $(OBJDUMP) -d -j .text --no-show-raw-insn $(KERNEL) | \
		grep -Ei '%(xmm|ymm|zmm|mm|k)[0-9]+|^[[:space:]]*[0-9a-f]+:[[:space:]]+(f[a-z0-9]+|emms|fxsave|fxrstor|ldmxcsr|stmxcsr|v[a-z0-9]+)([[:space:]]|$$)' | \
		grep -Ev '[[:space:]](verr|verw)[[:space:]]' || true )"; \
		test -z "$$forbidden" || { echo 'kernel contains floating-point, MMX, SSE, or AVX instructions'; echo "$$forbidden"; exit 1; }
	@$(NM) $(KERNEL) | grep -Eq ' [ABDRTt] __text_start$$'
	@$(NM) $(KERNEL) | grep -Eq ' [ABDRTt] __rodata_start$$'
	@$(NM) $(KERNEL) | grep -Eq ' [ABDRTt] __data_start$$'
	# The Rust half has to actually be in the image, and has to have been
	# linked as ordinary code rather than as something with its own runtime.
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_logo_decode$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_logo_self_test$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_font_glyph$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_font_self_test$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_ui_font_glyph$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_ui_font_self_test$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_fat16_parse_bpb$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_fat16_find_root$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_fat16_parse_fat$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_fat16_validate_extent$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_fat16_validate_payload$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_fat32_parse_bpb$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_fat32_parse_fsinfo$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_fat32_validate_fat_pair$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_fat32_parse_directory_entry$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_linux_fat16_find_root$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_linux_fat16_build_chain$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_linux_fat16_validate_payload$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_linux_elf64_parse$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_linux_elf64_self_test$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_linux_uname_fat16_find_root$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_linux_uname_fat16_build_chain$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_linux_uname_fat16_validate_payload$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_linux_uname_elf64_parse$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_linux_uname_elf64_self_test$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_linux_cat_fat16_find_root$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_linux_cat_fat16_build_chain$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_linux_cat_fat16_validate_payload$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_linux_cat_elf64_parse$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_linux_cat_elf64_self_test$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_elf64_parse$$'
	@$(NM) $(KERNEL) | grep -Eq ' T sapote_elf64_self_test$$'
	@if $(NM) $(KERNEL) | grep -Eq 'panic_bounds_check'; then \
		echo 'safe Rust retained a reachable bounds-panic path'; exit 1; \
	fi
	# Paging and the scenario runner must stay coupled to one typed aggregate,
	# never grow hardware-specific parameters or hidden firmware reads again.
	@grep -Fq 'paging_initialize(const struct paging_device_windows *windows);' \
		include/sapote/paging.h
	@! grep -Eq 'struct (acpi_topology|acpi_mcfg|boot_framebuffer)' \
		src/kernel/paging.c
	@grep -Fq 'const struct kernel_test_context *context' \
		include/sapote/test.h
	# Migrated boot operations are reachable only from typed ledger descriptors.
	@if grep -ERn \
		'\b(prove_frame_lifecycle|install_page_tables|prove_paging_lifecycle|prove_write_combining|bring_up_heap|prove_heap_lifecycle|prove_timer_route|retire_legacy_interrupt_path|prove_level_route|prove_pm_timer|prove_apic_timer|prove_tsc|retire_pit|prove_clocks_without_pit|prove_monotonic_time|bring_up_pci|prove_threads|prove_preemption|prove_framebuffer|prove_surface|draw_logo|prove_screen_console|prove_keyboard|prove_shell)[[:space:]]*[(]' \
		src/kernel --include='*.c' --exclude=boot_plan.c --exclude=boot_proofs.c; then \
		echo 'migrated boot stage bypasses the Boot Ledger'; exit 1; \
	fi
	@if grep -ERn \
		'\b(ui_font_initialize|pointer_initialize|ui_construct|ui_activate)[[:space:]]*[(]' \
		src/kernel --include='*.c' --exclude=boot_plan.c \
		--exclude=ui.c --exclude=ui_font.c --exclude=pointer.c; then \
		echo 'First Light boot stage bypasses the Boot Ledger'; exit 1; \
	fi
	@if grep -ERn \
		'\b(pci_resource_initialize|interrupt_vector_initialize|dma_initialize|device_substrate_prove)[[:space:]]*[(]' \
		src/kernel --include='*.c' --exclude=boot_plan.c \
		--exclude=pci_resource.c --exclude=interrupt_vector.c \
		--exclude=dma.c --exclude=virtio_rng_proof.c; then \
		echo 'device foundation boot operation bypasses the Boot Ledger'; exit 1; \
	fi
	@if grep -ERn '\bxhci_descriptor_prove[[:space:]]*[(]' \
		src/kernel --include='*.c' --exclude=boot_plan.c --exclude=xhci.c; then \
		echo 'xHCI descriptor proof bypasses the Boot Ledger'; exit 1; \
	fi
	@if grep -ERn '\bnvme_read_prove[[:space:]]*[(]' \
		src/kernel --include='*.c' --exclude=boot_plan.c --exclude=nvme.c; then \
		echo 'NVMe read proof bypasses the Boot Ledger'; exit 1; \
	fi
	@if grep -ERn '\bfilesystem_file_prove[[:space:]]*[(]' \
		src/kernel --include='*.c' --exclude=boot_plan.c \
		--exclude=filesystem.c; then \
		echo 'filesystem file proof bypasses the Boot Ledger'; exit 1; \
	fi
	@if grep -ERn '\bprocess_installed_prove[[:space:]]*[(]' \
		src/kernel --include='*.c' --exclude=boot_plan.c \
		--exclude=process.c; then \
		echo 'process proof bypasses the Boot Ledger'; exit 1; \
	fi
	@if grep -ERn '\blinux_abi_installed_prove[[:space:]]*[(]' \
		src/kernel --include='*.c' --exclude=boot_plan.c \
		--exclude=linux_abi.c; then \
		echo 'Linux ABI proof bypasses the Boot Ledger'; exit 1; \
	fi
	@if grep -ERn '\blinux_uname_abi_installed_prove[[:space:]]*[(]' \
		src/kernel --include='*.c' --exclude=boot_plan.c \
		--exclude=linux_uname.c; then \
		echo 'Linux uname ABI proof bypasses the Boot Ledger'; exit 1; \
	fi
	@test "$$(grep -ERh '\blinux_abi_launch[[:space:]]*[(]' \
		src/kernel --include='*.c' --exclude=linux_abi.c | wc -l)" -eq 1 && \
		test "$$(grep -ERh '\blinux_uname_abi_launch[[:space:]]*[(]' \
		src/kernel --include='*.c' --exclude=linux_uname.c | wc -l)" -eq 1 && \
		test "$$(grep -ERh '\blinux_cat_abi_launch[[:space:]]*[(]' \
		src/kernel --include='*.c' --exclude=linux_cat.c | wc -l)" -eq 1 || \
		{ echo 'measured launch entry escaped its userspace owner'; exit 1; }
	@! grep -Eq 'console_(write|putc)[[:space:]]*\([[:space:]]*"(SAPOTE|Linux)' \
		src/kernel/shell.c || \
		{ echo 'First Light shell contains prerecorded userspace output'; exit 1; }
	@if grep -ERn '\bfilesystem_private_read_(open|close)[[:space:]]*[(]' \
		src/kernel --include='*.c' --exclude=filesystem.c \
		--exclude=process.c; then \
		echo 'private one-file read seam escaped the process owner'; exit 1; \
	fi
	@if grep -ERn '\bfilesystem_linux_read_(open|close)[[:space:]]*[(]' \
		src/kernel --include='*.c' --exclude=filesystem.c \
		--exclude=linux_abi.c; then \
		echo 'private BusyBox read seam escaped the Linux process owner'; exit 1; \
	fi
	@if grep -ERn '\bfilesystem_linux_uname_read_(open|close)[[:space:]]*[(]' \
		src/kernel --include='*.c' --exclude=filesystem.c \
		--exclude=linux_uname.c; then \
		echo 'private uname BusyBox read seam escaped its process owner'; exit 1; \
	fi
	@if grep -ERn '\bfilesystem_linux_cat_read_(open|close)[[:space:]]*[(]' \
		src/kernel --include='*.c' --exclude=filesystem.c \
		--exclude=linux_cat.c; then \
		echo 'private cat BusyBox read seam escaped its process owner'; exit 1; \
	fi
	@if grep -ERn '\bpaging_process_table_failure_(arm|result|disarm|armed)[[:space:]]*[(]' \
		src/kernel --include='*.c' --exclude=paging.c \
		--exclude=linux_abi.c --exclude=linux_uname.c \
		--exclude=linux_cat.c; then \
		echo 'private paging failure control escaped the Linux process owner'; \
		exit 1; \
	fi
	@if grep -ERn '\bnvme_filesystem_session_(open|read|view|close)[[:space:]]*[(]' \
		src/kernel --include='*.c' --exclude=filesystem.c --exclude=nvme.c; then \
		echo 'private filesystem read session escaped its owner'; exit 1; \
	fi
	@grep -Fq '#define NVME_NVM_WRITE UINT8_C(0x01)' src/kernel/nvme.c
	@test "$$(grep -ERh '\bnvme_volume_write[[:space:]]*[(]' \
		src/kernel --include='*.c' --exclude=nvme.c | wc -l)" -eq 1 && \
		grep -Fq 'nvme_volume_write(session, sector, data,' \
			src/kernel/fat32_fs.c || \
		{ echo 'NVMe write access escaped the FAT32 owner'; exit 1; }
	@test "$$(grep -Ec 'process_return_interrupt[[:space:]]*[(]' \
		src/kernel/process.c)" -eq 1 && \
		grep -Fq 'interrupt_process_gate_arm(process_return_interrupt,' \
		src/kernel/process.c || \
		{ echo 'process proof return handler has an unexpected call site'; exit 1; }
	@test "$$($(NM) $(KERNEL) | grep -Ec ' T linux_syscall_entry$$')" -eq 1 || \
		{ echo 'Linux proof has no unique architectural syscall entry'; exit 1; }
	@$(OBJDUMP) -d --no-show-raw-insn $(BUSYBOX_BINARY) \
		| grep -Eq '[[:space:]]syscall[[:space:]]*$$' || \
		{ echo 'pinned BusyBox has no x86-64 syscall instruction'; exit 1; }
	@$(OBJDUMP) -d --no-show-raw-insn $(BUSYBOX_UNAME_BINARY) \
		| grep -Eq '[[:space:]]syscall[[:space:]]*$$' || \
		{ echo 'pinned uname BusyBox has no x86-64 syscall instruction'; exit 1; }
	@$(OBJDUMP) -d --no-show-raw-insn $(BUSYBOX_CAT_BINARY) \
		| grep -Eq '[[:space:]]syscall[[:space:]]*$$' || \
		{ echo 'pinned cat BusyBox has no x86-64 syscall instruction'; exit 1; }
	@if grep -ERn '(^|[^[:alnum:]_])unsafe[[:space:]]*(\{|fn|extern|trait|impl)|#\[unsafe' \
		src/rust --include='*.rs' --exclude=abi.rs; then \
		echo 'unsafe Rust escaped the reviewed FFI boundary'; exit 1; \
	fi
	@! grep -Eq '\*const|\*mut' src/rust/fat16.rs || \
		{ echo 'safe FAT16 parser retained or exposed a raw pointer'; exit 1; }
	@! grep -Eq '\*const|\*mut' src/rust/linux_fat16.rs || \
		{ echo 'safe Linux FAT16 parser retained or exposed a raw pointer'; exit 1; }
	@! grep -Eq '\*const|\*mut' src/rust/linux_elf64.rs || \
		{ echo 'safe Linux ELF64 parser retained or exposed a raw pointer'; exit 1; }
	@! grep -Eq '\*const|\*mut' src/rust/elf64.rs || \
		{ echo 'safe ELF64 parser retained or exposed a raw pointer'; exit 1; }
	@grep -Fq 'case KERNEL_TEST_DEVICE_SUBSTRATE:' src/kernel/test.c
	@grep -Fq '        return UINT8_C(0x30);' src/kernel/test.c
	@guest_exit=$$(sed -n \
		'/case KERNEL_TEST_DEVICE_SUBSTRATE:/{n;s/.*UINT8_C(\(0x[0-9A-Fa-f]*\)).*/\1/p;}' \
		src/kernel/test.c); \
		host_exit=$$(sed -n \
		's/^[[:space:]]*device-substrate) expected=\([0-9][0-9]*\) ;;.*/\1/p' \
		Makefile | head -n 1); \
		test -n "$$guest_exit" && test -n "$$host_exit" && \
		test "$$((guest_exit * 2 + 1))" -eq "$$host_exit" || \
		{ echo 'device-substrate guest and host exits disagree'; exit 1; }
	@test "$$(grep -Ec 'proof_interrupt[[:space:]]*[(]' \
		src/kernel/virtio_rng_proof.c)" -eq 1 || \
		{ echo 'VirtIO proof directly injects its MSI-X handler'; exit 1; }
	@grep -Fq 'case KERNEL_TEST_XHCI:' src/kernel/test.c
	@grep -Fq '        return UINT8_C(0x31);' src/kernel/test.c
	@guest_exit=$$(sed -n \
		'/case KERNEL_TEST_XHCI:/{n;s/.*UINT8_C(\(0x[0-9A-Fa-f]*\)).*/\1/p;}' \
		src/kernel/test.c); \
		host_exit=$$(sed -n \
		's/^[[:space:]]*xhci) expected=\([0-9][0-9]*\) ;;.*/\1/p' \
		Makefile | head -n 1); \
		test -n "$$guest_exit" && test -n "$$host_exit" && \
		test "$$((guest_exit * 2 + 1))" -eq "$$host_exit" || \
		{ echo 'xHCI guest and host exits disagree'; exit 1; }
	@test "$$(grep -Ec 'xhci_interrupt_handler[[:space:]]*[(]' \
		src/kernel/xhci.c)" -eq 1 || \
		{ echo 'xHCI proof directly injects its MSI-X handler'; exit 1; }
	@grep -Fq 'case KERNEL_TEST_NVME:' src/kernel/test.c
	@grep -Fq '        return UINT8_C(0x32);' src/kernel/test.c
	@guest_exit=$$(sed -n \
		'/case KERNEL_TEST_NVME:/{n;s/.*UINT8_C(\(0x[0-9A-Fa-f]*\)).*/\1/p;}' \
		src/kernel/test.c); \
		host_exit=$$(sed -n \
		's/^[[:space:]]*nvme) expected=\([0-9][0-9]*\) ;;.*/\1/p' \
		Makefile | head -n 1); \
		test -n "$$guest_exit" && test -n "$$host_exit" && \
		test "$$((guest_exit * 2 + 1))" -eq "$$host_exit" && \
		test "$$((0x33 * 2 + 1))" -ne "$$host_exit" || \
		{ echo 'NVMe guest and host exit contracts disagree'; exit 1; }
	@test "$$(grep -Ec 'nvme_interrupt_handler[[:space:]]*[(]' \
		src/kernel/nvme.c)" -eq 1 || \
		{ echo 'NVMe proof directly injects its MSI-X handler'; exit 1; }
	@grep -Fq 'case KERNEL_TEST_FILESYSTEM:' src/kernel/test.c
	@grep -Fq '        return UINT8_C(0x33);' src/kernel/test.c
	@guest_exit=$$(sed -n \
		'/case KERNEL_TEST_FILESYSTEM:/{n;s/.*UINT8_C(\(0x[0-9A-Fa-f]*\)).*/\1/p;}' \
		src/kernel/test.c); \
		host_exit=$$(sed -n \
		's/^[[:space:]]*filesystem) expected=\([0-9][0-9]*\) ;;.*/\1/p' \
		Makefile | head -n 1); \
		test -n "$$guest_exit" && test -n "$$host_exit" && \
		test "$$((guest_exit * 2 + 1))" -eq "$$host_exit" && \
		test "$$((0x34 * 2 + 1))" -ne "$$host_exit" || \
		{ echo 'filesystem guest and host exit contracts disagree'; exit 1; }
	@grep -Fq 'case KERNEL_TEST_PROCESS:' src/kernel/test.c
	@grep -Fq '        return UINT8_C(0x34);' src/kernel/test.c
	@guest_exit=$$(sed -n \
		'/case KERNEL_TEST_PROCESS:/{n;s/.*UINT8_C(\(0x[0-9A-Fa-f]*\)).*/\1/p;}' \
		src/kernel/test.c); \
		host_exit=$$(sed -n \
		's/^[[:space:]]*process) expected=\([0-9][0-9]*\) ;;.*/\1/p' \
		Makefile | head -n 1); \
		test -n "$$guest_exit" && test -n "$$host_exit" && \
		test "$$((guest_exit * 2 + 1))" -eq "$$host_exit" && \
		test "$$((0x33 * 2 + 1))" -ne "$$host_exit" || \
		{ echo 'process guest and host exit contracts disagree'; exit 1; }
	@grep -Fq 'case KERNEL_TEST_LINUX_ABI:' src/kernel/test.c
	@grep -Fq '        return UINT8_C(0x36);' src/kernel/test.c
	@guest_exit=$$(sed -n \
		'/case KERNEL_TEST_LINUX_ABI:/{n;s/.*UINT8_C(\(0x[0-9A-Fa-f]*\)).*/\1/p;}' \
		src/kernel/test.c); \
		host_exit=$$(sed -n \
		's/^[[:space:]]*linux-abi) expected=\([0-9][0-9]*\) ;;.*/\1/p' \
		Makefile | head -n 1); \
		test -n "$$guest_exit" && test -n "$$host_exit" && \
		test "$$((guest_exit * 2 + 1))" -eq "$$host_exit" && \
		test "$$((0x35 * 2 + 1))" -ne "$$host_exit" || \
		{ echo 'Linux ABI guest and host exit contracts disagree'; exit 1; }
	@grep -Fq 'case KERNEL_TEST_LINUX_ABI_UNAME:' src/kernel/test.c
	@grep -Fq '        return UINT8_C(0x37);' src/kernel/test.c
	@guest_exit=$$(sed -n \
		'/case KERNEL_TEST_LINUX_ABI_UNAME:/{n;s/.*UINT8_C(\(0x[0-9A-Fa-f]*\)).*/\1/p;}' \
		src/kernel/test.c); \
		host_exit=$$(sed -n \
		's/^[[:space:]]*linux-abi-uname) expected=\([0-9][0-9]*\) ;;.*/\1/p' \
		Makefile | head -n 1); \
		test -n "$$guest_exit" && test -n "$$host_exit" && \
		test "$$((guest_exit * 2 + 1))" -eq "$$host_exit" && \
		test "$$((0x36 * 2 + 1))" -ne "$$host_exit" || \
		{ echo 'Linux uname ABI guest and host exit contracts disagree'; exit 1; }
	@grep -Fq 'case KERNEL_TEST_FIRST_LIGHT_USERLAND:' src/kernel/test.c
	@grep -Fq '        return UINT8_C(0x38);' src/kernel/test.c
	@guest_exit=$$(sed -n \
		'/case KERNEL_TEST_FIRST_LIGHT_USERLAND:/{n;s/.*UINT8_C(\(0x[0-9A-Fa-f]*\)).*/\1/p;}' \
		src/kernel/test.c); \
		host_exit=$$(sed -n \
		's/^[[:space:]]*first-light-userland) expected=\([0-9][0-9]*\) ;;.*/\1/p' \
		Makefile | head -n 1); \
		test -n "$$guest_exit" && test -n "$$host_exit" && \
		test "$$((guest_exit * 2 + 1))" -eq "$$host_exit" || \
		{ echo 'First Light userland guest and host exits disagree'; exit 1; }
	@grep -Fq 'case KERNEL_TEST_FIRST_LIGHT_USERLAND_ABSENT:' src/kernel/test.c
	@grep -Fq '        return UINT8_C(0x39);' src/kernel/test.c
	@guest_exit=$$(sed -n \
		'/case KERNEL_TEST_FIRST_LIGHT_USERLAND_ABSENT:/{n;s/.*UINT8_C(\(0x[0-9A-Fa-f]*\)).*/\1/p;}' \
		src/kernel/test.c); \
		host_exit=$$(sed -n \
		's/^[[:space:]]*first-light-userland-absent) expected=\([0-9][0-9]*\) ;;.*/\1/p' \
		Makefile | head -n 1); \
		test -n "$$guest_exit" && test -n "$$host_exit" && \
		test "$$((guest_exit * 2 + 1))" -eq "$$host_exit" || \
		{ echo 'First Light absent-volume guest and host exits disagree'; exit 1; }
	@grep -Fq 'case KERNEL_TEST_FIRST_LIGHT_USERLAND_INTERACTIVE:' src/kernel/test.c
	@grep -Fq '        return UINT8_C(0x3A);' src/kernel/test.c
	@guest_exit=$$(sed -n \
		'/case KERNEL_TEST_FIRST_LIGHT_USERLAND_INTERACTIVE:/{n;s/.*UINT8_C(\(0x[0-9A-Fa-f]*\)).*/\1/p;}' \
		src/kernel/test.c); \
		host_exit=$$(sed -n \
		's/^[[:space:]]*first-light-userland-interactive) expected=\([0-9][0-9]*\) ;;.*/\1/p' \
		Makefile | head -n 1); \
		test -n "$$guest_exit" && test -n "$$host_exit" && \
		test "$$((guest_exit * 2 + 1))" -eq "$$host_exit" || \
		{ echo 'Interactive First Light guest and host exits disagree'; exit 1; }
	@grep -Fq 'case KERNEL_TEST_FIRST_LIGHT_USERLAND_INTERACTIVE_ABSENT:' src/kernel/test.c
	@grep -Fq '        return UINT8_C(0x3B);' src/kernel/test.c
	@guest_exit=$$(sed -n \
		'/case KERNEL_TEST_FIRST_LIGHT_USERLAND_INTERACTIVE_ABSENT:/{n;s/.*UINT8_C(\(0x[0-9A-Fa-f]*\)).*/\1/p;}' \
		src/kernel/test.c); \
		host_exit=$$(sed -n \
		's/^[[:space:]]*first-light-userland-interactive-absent) expected=\([0-9][0-9]*\) ;;.*/\1/p' \
		Makefile | head -n 1); \
		test -n "$$guest_exit" && test -n "$$host_exit" && \
		test "$$((guest_exit * 2 + 1))" -eq "$$host_exit" || \
		{ echo 'Interactive absent-profile guest and host exits disagree'; exit 1; }
	@if grep -En '\bframebuffer_(write_pixel|fill|scroll_up)[[:space:]]*[(]' \
		src/kernel/ui.c src/kernel/ui_font.c src/kernel/pointer.c; then \
		echo 'First Light bypasses the cached surface'; exit 1; \
	fi
	@if grep -En \
		'\b(ui_process_events|ui_flush|surface_present)[[:space:]]*[(]' \
		src/kernel/pointer.c; then \
		echo 'PS/2 pointer interrupt path attempts UI drawing'; exit 1; \
	fi
	@grep -Fq '    cpu_store_fence();' src/kernel/surface.c || \
		{ echo 'cached-surface WC present lost its sfence'; exit 1; }
	@grep -Fq 'Sapote: First Light installed proof passed' \
		src/kernel/boot_plan.c
	$(MAKE) screenshot-proof

screenshot-proof:
	$(PYTHON) tools/compare-first-light-screenshot.py --mode clean \
		--self-test $(FIRST_LIGHT_IMAGE)
	$(PYTHON) tools/compare-first-light-screenshot.py --mode focus \
		--self-test $(FIRST_LIGHT_FOCUS_IMAGE)
	$(PYTHON) tools/compare-first-light-screenshot.py --mode terminal \
		--self-test $(FIRST_LIGHT_TERMINAL_IMAGE)

capture-first-light: iso $(FAT32_SYSTEM_IMAGE) $(FAT32_DATA_IMAGE)
	rm -rf $(FIRST_LIGHT_CAPTURE_DIR)
	$(PYTHON) tools/capture-first-light.py --iso $(ISO) \
		--system $(FAT32_SYSTEM_IMAGE) --data $(FAT32_DATA_IMAGE) \
		--output $(FIRST_LIGHT_CAPTURE_DIR)
	$(PYTHON) tools/compare-first-light-screenshot.py --mode clean \
		$(FIRST_LIGHT_IMAGE) $(FIRST_LIGHT_CAPTURE_DIR)/sapote-first-light.png
	$(PYTHON) tools/compare-first-light-screenshot.py --mode focus \
		$(FIRST_LIGHT_FOCUS_IMAGE) \
		$(FIRST_LIGHT_CAPTURE_DIR)/sapote-first-light-focus.png
	$(PYTHON) tools/compare-first-light-screenshot.py --mode terminal \
		$(FIRST_LIGHT_TERMINAL_IMAGE) \
		$(FIRST_LIGHT_CAPTURE_DIR)/sapote-first-light-terminal.png

capture-first-environment: iso $(FAT32_SYSTEM_IMAGE) $(FAT32_DATA_IMAGE)
	rm -rf $(FIRST_ENVIRONMENT_CAPTURE_DIR)
	$(PYTHON) tools/capture-first-environment.py --iso $(ISO) \
		--system $(FAT32_SYSTEM_IMAGE) --data $(FAT32_DATA_IMAGE) \
		--output $(FIRST_ENVIRONMENT_CAPTURE_DIR) --ffmpeg $(FFMPEG)
	cp $(FIRST_ENVIRONMENT_CAPTURE_DIR)/sapote-first-environment.png \
		$(FIRST_ENVIRONMENT_IMAGE)
	cp $(FIRST_ENVIRONMENT_CAPTURE_DIR)/sapote-first-environment-dock.png \
		$(FIRST_ENVIRONMENT_DOCK_IMAGE)
	cp $(FIRST_ENVIRONMENT_CAPTURE_DIR)/sapote-first-environment-terminal.png \
		$(FIRST_ENVIRONMENT_TERMINAL_IMAGE)
	cp $(FIRST_ENVIRONMENT_CAPTURE_DIR)/sapote-first-environment-files.png \
		$(FIRST_ENVIRONMENT_FILES_IMAGE)
	cp $(FIRST_ENVIRONMENT_CAPTURE_DIR)/sapote-first-environment-notes.png \
		$(FIRST_ENVIRONMENT_NOTES_IMAGE)
	cp $(FIRST_ENVIRONMENT_CAPTURE_DIR)/sapote-first-environment-studio.png \
		$(FIRST_ENVIRONMENT_STUDIO_IMAGE)
	cp $(FIRST_ENVIRONMENT_CAPTURE_DIR)/sapote-first-environment-20s.mp4 \
		$(FIRST_ENVIRONMENT_VIDEO)

capture-boot-video: iso $(FAT32_SYSTEM_IMAGE) $(FAT32_DATA_IMAGE)
	cp $(FAT32_DATA_IMAGE) $(BUILD_DIR)/capture-video-data-fat32.raw
	$(PYTHON) tools/capture-fat32-persistence.py --iso $(ISO) \
		--system $(FAT32_SYSTEM_IMAGE) \
		--data $(BUILD_DIR)/capture-video-data-fat32.raw \
		--screenshot $(BUILD_DIR)/fat32-persistence.png \
		--video $(FIRST_LIGHT_BOOT_VIDEO) \
		--transcript $(BUILD_DIR)/fat32-persistence.log \
		--ffmpeg $(FFMPEG)

$(ISO): $(KERNEL) grub/grub.cfg
	mkdir -p $(ISO_ROOT)/boot/grub
	cp $(KERNEL) $(ISO_ROOT)/boot/sapote.elf
	cp grub/grub.cfg $(ISO_ROOT)/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) $(GRUB_MKRESCUE_FLAGS) -o $@ $(ISO_ROOT)

iso: $(ISO)

$(TEST_BUILD_DIR)/%/sapote.iso: $(KERNEL) Makefile
	rm -rf $(TEST_BUILD_DIR)/$*
	mkdir -p $(TEST_BUILD_DIR)/$*/iso-root/boot/grub
	cp $(KERNEL) $(TEST_BUILD_DIR)/$*/iso-root/boot/sapote.elf
	printf '%s\n' 'set default=0' 'set timeout=0' '' \
		'menuentry "Sapote test" {' \
		'    multiboot2 /boot/sapote.elf sapote.test=$*' \
		'    boot' '}' >$(TEST_BUILD_DIR)/$*/iso-root/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) $(GRUB_MKRESCUE_FLAGS) -o $@ $(TEST_BUILD_DIR)/$*/iso-root

qemu-test-%: $(TEST_BUILD_DIR)/%/sapote.iso
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
		device-windows) expected=91 ;; \
		boot-ledger) expected=93 ;; \
		first-light) expected=95 ;; \
		device-substrate) expected=97 ;; \
		xhci) expected=99 ;; \
		nvme) expected=101 ;; \
		filesystem) expected=103 ;; \
		process) expected=105 ;; \
		linux-abi) expected=109 ;; \
		linux-abi-uname) expected=111 ;; \
		first-light-userland) expected=113 ;; \
		first-light-userland-absent) expected=115 ;; \
		first-light-userland-interactive) expected=117 ;; \
		first-light-userland-interactive-absent) expected=119 ;; \
		fat32-system) expected=121 ;; \
		fat32-data) expected=123 ;; \
		fat32-nested) expected=125 ;; \
		fat32-growth) expected=127 ;; \
		fat32-random) expected=129 ;; \
		fat32-truncate) expected=131 ;; \
		fat32-rename) expected=133 ;; \
		fat32-delete) expected=135 ;; \
		fat32-full) expected=137 ;; \
		fat32-corrupt) expected=139 ;; \
		fat32-missing) expected=141 ;; \
		fat32-persistence) expected=143 ;; \
		fat32-cache) expected=145 ;; \
		fat32-immutable) expected=147 ;; \
		fat32-handles) expected=149 ;; \
		*) echo 'unknown QEMU scenario: $*'; exit 1 ;; \
	esac; \
		# The ECAM and device-window scenarios depart from the default machine. \
		# i440fx publishes no \
		# MCFG, so every other scenario - including pci - proves the path that \
		# has nothing but the I/O ports. q35 is the only machine here with a \
		# PCI Express host bridge, and the root port is what gives the \
		# enumeration a second bus to find. Both PCI scenarios name their \
		# network device explicitly instead of relying on QEMU defaults. \
		case '$*' in \
			pci) hardware='-device e1000e' ;; \
			pci-ecam) \
				hardware='-machine q35 -device pcie-root-port,id=rp0,chassis=1 -device e1000e,bus=rp0 -device e1000e' ;; \
			device-windows) hardware='-machine q35' ;; \
			device-substrate) \
				hardware='-object rng-builtin,id=rng0 -device virtio-rng-pci,disable-legacy=on,rng=rng0' ;; \
			xhci) \
				hardware='-device qemu-xhci,id=xhci,streams=off -device usb-kbd,bus=xhci.0,port=1,usb_version=2' ;; \
			nvme) \
				rm -f '$(NVME_FIXTURE)' || exit 1; \
				$(PYTHON) tools/make-nvme-fixture.py '$(NVME_FIXTURE)' || exit 1; \
				test -f '$(NVME_FIXTURE)' || exit 1; \
				hardware='-blockdev driver=file,filename=$(NVME_FIXTURE),node-name=nvme-file,read-only=on,auto-read-only=off -blockdev driver=raw,file=nvme-file,node-name=nvme-raw,read-only=on -device nvme,serial=sapote-fixture,drive=nvme-raw,logical_block_size=4096,physical_block_size=4096,max_ioqpairs=1,msix_qsize=1' ;; \
			filesystem) \
				rm -f '$(FILESYSTEM_FIXTURE)' || exit 1; \
				$(PYTHON) tools/make-fat16-fixture.py '$(FILESYSTEM_FIXTURE)' || exit 1; \
				test -f '$(FILESYSTEM_FIXTURE)' || exit 1; \
				hardware='-boot order=d -blockdev driver=file,filename=$(FILESYSTEM_FIXTURE),node-name=filesystem-file,read-only=on,auto-read-only=off -blockdev driver=raw,file=filesystem-file,node-name=filesystem-raw,read-only=on -device nvme,serial=sapote-fat16-fixture,drive=filesystem-raw,logical_block_size=4096,physical_block_size=4096,max_ioqpairs=1,msix_qsize=1' ;; \
			process) \
				rm -f '$(PROCESS_FIXTURE)' '$(PROCESS_ELF)' || exit 1; \
				$(PYTHON) tools/make-process-fixture.py '$(PROCESS_FIXTURE)' || exit 1; \
				test -f '$(PROCESS_FIXTURE)' || exit 1; \
				hardware='-boot order=d -blockdev driver=file,filename=$(PROCESS_FIXTURE),node-name=process-file,read-only=on,auto-read-only=off -blockdev driver=raw,file=process-file,node-name=process-raw,read-only=on -device nvme,serial=sapote-process,drive=process-raw,logical_block_size=4096,physical_block_size=4096,max_ioqpairs=1,msix_qsize=1' ;; \
			linux-abi) \
				$(MAKE) '$(LINUX_ABI_FIXTURE)' || exit 1; \
				test -f '$(LINUX_ABI_FIXTURE)' || exit 1; \
				hardware='-boot order=d -blockdev driver=file,filename=$(LINUX_ABI_FIXTURE),node-name=linux-abi-file,read-only=on,auto-read-only=off -blockdev driver=raw,file=linux-abi-file,node-name=linux-abi-raw,read-only=on -device nvme,serial=sapote-linux-abi,drive=linux-abi-raw,logical_block_size=4096,physical_block_size=4096,max_ioqpairs=1,msix_qsize=1' ;; \
			linux-abi-uname) \
				$(MAKE) '$(LINUX_UNAME_FIXTURE)' || exit 1; \
				test -f '$(LINUX_UNAME_FIXTURE)' || exit 1; \
				hardware='-boot order=d -blockdev driver=file,filename=$(LINUX_UNAME_FIXTURE),node-name=linux-uname-file,read-only=on,auto-read-only=off -blockdev driver=raw,file=linux-uname-file,node-name=linux-uname-raw,read-only=on -device nvme,serial=sapote-linux-uname,drive=linux-uname-raw,logical_block_size=4096,physical_block_size=4096,max_ioqpairs=1,msix_qsize=1' ;; \
			first-light-userland) \
				$(MAKE) '$(FIRST_LIGHT_USERLAND_IMAGE)' || exit 1; \
				test -f '$(FIRST_LIGHT_USERLAND_IMAGE)' || exit 1; \
				hardware='-boot order=d -blockdev driver=file,filename=$(FIRST_LIGHT_USERLAND_IMAGE),node-name=first-light-userland-file,read-only=on,auto-read-only=off -blockdev driver=raw,file=first-light-userland-file,node-name=first-light-userland-raw,read-only=on -device nvme,serial=sapote-userland,drive=first-light-userland-raw,logical_block_size=4096,physical_block_size=4096,max_ioqpairs=1,msix_qsize=1' ;; \
			first-light-userland-interactive) \
				$(MAKE) '$(FIRST_LIGHT_USERLAND_IMAGE)' || exit 1; \
				test -f '$(FIRST_LIGHT_USERLAND_IMAGE)' || exit 1; \
				hardware='-boot order=d -blockdev driver=file,filename=$(FIRST_LIGHT_USERLAND_IMAGE),node-name=interactive-userland-file,read-only=on,auto-read-only=off -blockdev driver=raw,file=interactive-userland-file,node-name=interactive-userland-raw,read-only=on -device nvme,serial=sapote-interactive,drive=interactive-userland-raw,logical_block_size=4096,physical_block_size=4096,max_ioqpairs=1,msix_qsize=1' ;; \
			first-light-userland-interactive-absent) \
				$(MAKE) '$(FIRST_LIGHT_USERLAND_NO_CAT_IMAGE)' || exit 1; \
				test -f '$(FIRST_LIGHT_USERLAND_NO_CAT_IMAGE)' || exit 1; \
				hardware='-boot order=d -blockdev driver=file,filename=$(FIRST_LIGHT_USERLAND_NO_CAT_IMAGE),node-name=interactive-absent-file,read-only=on,auto-read-only=off -blockdev driver=raw,file=interactive-absent-file,node-name=interactive-absent-raw,read-only=on -device nvme,serial=sapote-interactive-absent,drive=interactive-absent-raw,logical_block_size=4096,physical_block_size=4096,max_ioqpairs=1,msix_qsize=1' ;; \
			fat32-missing) \
				$(MAKE) '$(FAT32_SYSTEM_IMAGE)' || exit 1; \
				hardware='-boot order=d -blockdev driver=file,filename=$(FAT32_SYSTEM_IMAGE),node-name=fat32-system-file,read-only=on,auto-read-only=off -blockdev driver=raw,file=fat32-system-file,node-name=fat32-system-raw,read-only=on -device nvme,serial=sapote-system-fat32,drive=fat32-system-raw,logical_block_size=512,physical_block_size=512,max_ioqpairs=1,msix_qsize=1' ;; \
			fat32-full) \
				$(MAKE) '$(FAT32_SYSTEM_IMAGE)' '$(FAT32_FULL_IMAGE)' || exit 1; \
				cp '$(FAT32_FULL_IMAGE)' '$(TEST_BUILD_DIR)/$*/data.raw' || exit 1; \
				hardware='-boot order=d -blockdev driver=file,filename=$(FAT32_SYSTEM_IMAGE),node-name=fat32-system-file,read-only=on,auto-read-only=off -blockdev driver=raw,file=fat32-system-file,node-name=fat32-system-raw,read-only=on -device nvme,serial=sapote-system-fat32,drive=fat32-system-raw,logical_block_size=512,physical_block_size=512,max_ioqpairs=1,msix_qsize=1 -blockdev driver=file,filename=$(TEST_BUILD_DIR)/$*/data.raw,node-name=fat32-data-file,read-only=off,auto-read-only=off -blockdev driver=raw,file=fat32-data-file,node-name=fat32-data-raw,read-only=off -device nvme,serial=sapote-data-fat32,drive=fat32-data-raw,logical_block_size=512,physical_block_size=512,max_ioqpairs=1,msix_qsize=1' ;; \
			fat32-corrupt) \
				$(MAKE) '$(FAT32_SYSTEM_IMAGE)' '$(FAT32_CORRUPT_IMAGE)' || exit 1; \
				cp '$(FAT32_CORRUPT_IMAGE)' '$(TEST_BUILD_DIR)/$*/data.raw' || exit 1; \
				hardware='-boot order=d -blockdev driver=file,filename=$(FAT32_SYSTEM_IMAGE),node-name=fat32-system-file,read-only=on,auto-read-only=off -blockdev driver=raw,file=fat32-system-file,node-name=fat32-system-raw,read-only=on -device nvme,serial=sapote-system-fat32,drive=fat32-system-raw,logical_block_size=512,physical_block_size=512,max_ioqpairs=1,msix_qsize=1 -blockdev driver=file,filename=$(TEST_BUILD_DIR)/$*/data.raw,node-name=fat32-data-file,read-only=off,auto-read-only=off -blockdev driver=raw,file=fat32-data-file,node-name=fat32-data-raw,read-only=off -device nvme,serial=sapote-data-fat32,drive=fat32-data-raw,logical_block_size=512,physical_block_size=512,max_ioqpairs=1,msix_qsize=1' ;; \
			fat32-*) \
				$(MAKE) '$(FAT32_SYSTEM_IMAGE)' '$(FAT32_DATA_IMAGE)' || exit 1; \
				cp '$(FAT32_DATA_IMAGE)' '$(TEST_BUILD_DIR)/$*/data.raw' || exit 1; \
				hardware='-boot order=d -blockdev driver=file,filename=$(FAT32_SYSTEM_IMAGE),node-name=fat32-system-file,read-only=on,auto-read-only=off -blockdev driver=raw,file=fat32-system-file,node-name=fat32-system-raw,read-only=on -device nvme,serial=sapote-system-fat32,drive=fat32-system-raw,logical_block_size=512,physical_block_size=512,max_ioqpairs=1,msix_qsize=1 -blockdev driver=file,filename=$(TEST_BUILD_DIR)/$*/data.raw,node-name=fat32-data-file,read-only=off,auto-read-only=off -blockdev driver=raw,file=fat32-data-file,node-name=fat32-data-raw,read-only=off -device nvme,serial=sapote-data-fat32,drive=fat32-data-raw,logical_block_size=512,physical_block_size=512,max_ioqpairs=1,msix_qsize=1' ;; \
			*) hardware='' ;; \
	esac; \
	log='$(TEST_BUILD_DIR)/$*/serial.log'; \
	rm -f "$$log"; \
	timeout_seconds=15; reboot_control='-no-reboot'; \
	case '$*' in \
		fat32-*) timeout_seconds=45 ;; \
	esac; \
	if test '$*' = fat32-persistence; then reboot_control=''; fi; \
	set +e; \
	timeout "$${timeout_seconds}s" qemu-system-x86_64 \
		-machine accel=$(QEMU_ACCEL) -m 128M -smp 1 $$hardware \
		-cdrom '$<' -display none -monitor none -serial stdio \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		$$reboot_control >"$$log" 2>&1; result=$$?; \
	set -e; \
	begin_count=$$(grep -Fxc 'ST BEGIN $*' "$$log" || true); \
	pass_count=$$(grep -Fxc 'ST PASS $*' "$$log" || true); \
	expected_begin=1; \
	if test '$*' = fat32-persistence; then expected_begin=2; fi; \
	if test $$result -ne $$expected -o "$$begin_count" -ne "$$expected_begin" -o "$$pass_count" -ne 1 || \
		grep -Fq 'ST FAIL' "$$log" || grep -Fq 'Sapote PANIC' "$$log"; then \
		echo 'QEMU scenario $* failed: status='$$result' expected='$$expected; \
		cat "$$log"; \
		exit 1; \
	fi; \
	if test '$*' = normal && \
		{ ! grep -Fq 'Sapote: ACPI root verified' "$$log" || \
		  ! grep -Fq 'Sapote: ACPI MADT verified' "$$log" || \
		  ! grep -Fq 'Sapote: ACPI topology verified' "$$log" || \
		  ! grep -Eq '^Sapote: ACPI I/O APIC id [0-9]+ at 0x' "$$log" || \
		  ! grep -Fq 'Sapote: local APIC online' "$$log" || \
		  ! grep -Fq 'Sapote: local APIC legacy routing LINT0 ExtINT' "$$log" || \
		  ! grep -Eq '^Sapote: local APIC EOI-broadcast suppression (supported|unsupported) active (yes|no)$$' "$$log" || \
		  ! grep -Fq 'Sapote: I/O APIC online' "$$log" || \
		  ! grep -Eq '^Sapote: I/O APIC id [0-9]+ version 0x[0-9A-F]+ entries [0-9]+ base GSI [0-9]+ directed EOI (yes|no)$$' "$$log" || \
		  ! grep -Fq 'Sapote: I/O APIC delivered eight interrupts' "$$log" || \
		  ! grep -Fq 'Sapote: legacy 8259 retired' "$$log" || \
		  ! grep -Fq 'Sapote: timer survives legacy retirement' "$$log" || \
		  ! grep -Eq '^Sapote: I/O APIC level route id [0-9]+ GSI [0-9]+ vector [0-9]+ active (high|low) acknowledgement (directed|broadcast)$$' "$$log" || \
		  ! grep -Eq '^Sapote: I/O APIC level deliveries [0-9]+ remote IRR [0-9]+ directed EOI [0-9]+ in [0-9]+ ns$$' "$$log" || \
		  ! grep -Fq 'Sapote: I/O APIC delivered eight level-triggered interrupts' "$$log" || \
		  ! grep -Fq 'Sapote: level-triggered routing established' "$$log" || \
		  ! grep -Eq '^Sapote: local APIC timer calibrated at [0-9]+ counts' "$$log" || \
		  ! grep -Fq 'Sapote: local APIC timer delivered eight interrupts' "$$log" || \
		  ! grep -Eq '^Sapote: TSC calibrated at [0-9]+ Hz' "$$log" || \
		  ! grep -Fq 'Sapote: TSC reference established' "$$log" || \
		  ! grep -Fq 'Sapote: ACPI FADT verified' "$$log" || \
		  ! grep -Fq 'Sapote: ACPI MCFG absent' "$$log" || \
		  ! grep -Fq 'Sapote: ACPI configuration windows verified' "$$log" || \
		  ! grep -Eq '^Sapote: ACPI PM timer port 0x[0-9A-F]+ width (24|32) bits address (fixed|extended)$$' "$$log" || \
		  ! grep -Eq '^Sapote: PM timer counted [0-9]+ ticks in [0-9]+ ns$$' "$$log" || \
		  ! grep -Fq 'Sapote: PM timer independent reference established' "$$log" || \
		  ! grep -Eq '^Sapote: clocks agree: PM [0-9]+ ns, APIC timer [0-9]+ ns, TSC [0-9]+ ns$$' "$$log" || \
		  ! grep -Fq 'Sapote: PIT retired' "$$log" || \
		  ! grep -Fq 'Sapote: clocks survive PIT retirement' "$$log" || \
		  ! grep -Fq 'Sapote: monotonic clock on time-stamp counter' "$$log" || \
		  ! grep -Eq '^Sapote: slept [0-9]+ ns for a [0-9]+ ns deadline$$' "$$log" || \
		  ! grep -Fq 'Sapote: deadline timers online' "$$log" || \
		  ! grep -Fq 'Sapote: monotonic time established' "$$log" || \
		  ! grep -Eq '^Sapote: paging root 0x[0-9A-F]+ table frames [0-9]+ regions [0-9]+ NX yes write protect yes$$' "$$log" || \
		  ! grep -Eq '^Sapote: paging leaves [0-9]+ writable [0-9]+ executable [0-9]+ both 0$$' "$$log" || \
		  ! grep -Fq 'Sapote: kernel page tables installed' "$$log" || \
		  ! grep -Fq 'Sapote: no writable executable mapping' "$$log" || \
		  ! grep -Eq '^Sapote: IA32_PAT before 0x[0-9A-F]{16} after 0x[0-9A-F]{16} entry 1 write-combining$$' "$$log" || \
		  ! grep -Eq '^Sapote: framebuffer memory type write-combining pages [1-9][0-9]*$$' "$$log" || \
		  ! grep -Fq 'Sapote: write-combining established' "$$log" || \
		  ! grep -Fq 'Sapote: virtual memory established' "$$log" || \
		  ! grep -Eq '^Sapote: heap window 0x[0-9A-F]+ size [0-9]+ guards 0x[0-9A-F]+ 0x[0-9A-F]+$$' "$$log" || \
		  ! grep -Eq '^Sapote: heap committed [0-9]+ bytes in [0-9]+ pages, live 3$$' "$$log" || \
		  ! grep -Fq 'Sapote: kernel heap online' "$$log" || \
		  ! grep -Fq 'Sapote: heap coalesced to one free block' "$$log" || \
		  ! grep -Fq 'Sapote: kernel heap established' "$$log" || \
		  ! grep -Eq '^Sapote: deadline table of [0-9]+ entries on the heap$$' "$$log" || \
		  ! grep -Eq '^Sapote: PCI mechanism 1 online, no window mapped$$' "$$log" || \
		  ! grep -Eq '^Sapote: PCI buses [1-9][0-9]* functions [1-9][0-9]* bridges [0-9]+$$' "$$log" || \
		  ! grep -Eq '^Sapote: PCI 0:0\.0 vendor 0x[0-9A-F]+ device 0x[0-9A-F]+ class 0x0*6\.0x0* ' "$$log" || \
		  ! grep -Fq 'Sapote: PCI configuration space enumerated' "$$log" || \
		  ! grep -Fq 'Sapote: PCI enumeration established' "$$log" || \
		  ! grep -Fxq 'Sapote: PCI resource ownership negative controls 4/4 passed' "$$log" || \
		  ! grep -Fxq 'Sapote: supervisor NX UC device-MMIO arena established' "$$log" || \
		  ! grep -Fxq 'Sapote: dynamic vector negative controls 4/4 passed' "$$log" || \
		  ! grep -Fxq 'Sapote: dynamic interrupt vector foundation established' "$$log" || \
		  ! grep -Fxq 'Sapote: bounded DMA negative controls 2/2 passed' "$$log" || \
		  ! grep -Fxq 'Sapote: contiguous DMA ownership foundation established' "$$log" || \
		  ! grep -Fxq 'Sapote: xHCI foundation robustness controls 17/17 passed' "$$log" || \
		  ! grep -Fxq 'Sapote: bounded xHCI host-controller foundation established' "$$log" || \
		  ! grep -Fxq 'Sapote: xHCI fixture absent' "$$log" || \
		  ! grep -Fxq 'Sapote: NVMe foundation robustness controls 20/20 passed' "$$log" || \
		  ! grep -Fxq 'Sapote: bounded NVMe block-controller foundation established' "$$log" || \
		  ! grep -Fxq 'Sapote: NVMe fixture absent' "$$log" || \
		  ! grep -Fxq 'Sapote: FAT16 foundation robustness controls 26/26 passed' "$$log" || \
		  ! grep -Fxq 'Sapote: bounded read-only FAT16 foundation established' "$$log" || \
		  ! grep -Fxq 'Sapote: FAT16 fixture absent' "$$log" || \
		  ! grep -Fxq 'Sapote: process address-space foundation controls 8/8 passed' "$$log" || \
		  ! grep -Fxq 'Sapote: ELF64 parser robustness controls 34/34 passed' "$$log" || \
		  ! grep -Fxq 'Sapote: process fixture absent' "$$log" || \
		  ! grep -Fxq 'Sapote: Linux SYSCALL CPU foundation controls 10/10 passed' "$$log" || \
		  ! grep -Fxq 'Sapote: BusyBox image and Linux stack controls 32/32 passed' "$$log" || \
		  ! grep -Fxq 'Sapote: Linux ABI fixture absent' "$$log" || \
		  ! grep -Eq '^Sapote: threads online, 3 ready of [0-9]+ on 12 stack frames$$' "$$log" || \
		  ! grep -Fxq 'Sapote: thread rotation 123123123123' "$$log" || \
		  ! grep -Eq '^Sapote: threads switched [1-9][0-9]* times, 3 exited$$' "$$log" || \
		  ! grep -Fq 'Sapote: kernel threads established' "$$log" || \
		  ! grep -Eq '^Sapote: framebuffer [0-9]+x[0-9]+ at 0x[0-9A-F]+ pitch [0-9]+ RGB [0-9]+/[0-9]+/[0-9]+$$' "$$log" || \
		  ! grep -Fxq 'Sapote: framebuffer verified 786432 pixels' "$$log" || \
		  ! grep -Fq 'Sapote: framebuffer established' "$$log" || \
		  ! grep -Eq '^Sapote: surface [0-9]+x[0-9]+ pitch [0-9]+ buffer [0-9]+ bytes$$' "$$log" || \
		  ! grep -Eq '^Sapote: surface cycles full present [0-9]+ one-line update [0-9]+ scroll [0-9]+$$' "$$log" || \
		  ! grep -Eq '^Sapote: surface split cycles full draw [0-9]+ push [0-9]+ one-line draw [0-9]+ push [0-9]+ scroll draw [0-9]+ push [0-9]+$$' "$$log" || \
		  ! grep -Eq '^Sapote: surface sparse two-corner cycles total [0-9]+ draw [0-9]+ push [0-9]+ union [0-9]+$$' "$$log" || \
		  ! grep -Eq '^Sapote: surface copied [0-9]+ full, [0-9]+ line, [0-9]+ scroll pixels$$' "$$log" || \
		  ! grep -Fq 'Sapote: cached surface established' "$$log" || \
		  ! grep -Eq '^Sapote: screen console [0-9]+x[0-9]+ cells of 8x16, font [0-9]+ bytes$$' "$$log" || \
		  ! grep -Eq '^Sapote: screen console drew [0-9]+ characters and scrolled [0-9]+ times$$' "$$log" || \
		  ! grep -Fq 'Sapote: screen console established' "$$log" || \
		  ! grep -Fq 'Sapote: screen console passed' "$$log" || \
		  ! grep -Eq '^Sapote: keyboard 8042 online, IRQ 1 routed, [0-9]+ interrupts for [0-9]+ events$$' "$$log" || \
		  ! grep -Fxq 'Sapote: keyboard decoded "hiI" from injected scancodes' "$$log" || \
		  ! grep -Fq 'Sapote: keyboard established' "$$log" || \
		  ! grep -Fq 'Sapote: keyboard passed' "$$log" || \
		  ! grep -Fq 'Sapote: Boot Ledger installed proof passed' "$$log" || \
		  ! grep -Fq 'Sapote: First Light font verified' "$$log" || \
		  ! grep -Eq '^Sapote: PS/2 pointer (available|unavailable: .+)$$' "$$log" || \
		  ! grep -Fq 'Sapote: First Light layout validated' "$$log" || \
		  ! grep -Fq 'Sapote: First Light desktop constructed' "$$log" || \
		  ! grep -Fq 'Sapote: First Light desktop activated' "$$log" || \
		  ! grep -Fq 'Sapote: First Light installed proof passed' "$$log" || \
		  ! grep -Fxq 'Sapote: shell ran "echo hi" from 8 injected scancodes' "$$log" || \
		  ! grep -Fq 'Sapote: shell output verified on screen' "$$log" || \
		  ! grep -Fq 'Sapote: shell established' "$$log" || \
		  ! grep -Fq 'Sapote: shell passed' "$$log" || \
		  ! grep -Fq 'Sapote: never triple fault milestone passed' "$$log"; }; then \
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
			grep -Fq 'Sapote DOUBLE FAULT - HALTED' "$$log" || diagnostics_ok=false ;; \
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
			! grep -Fq 'Sapote: ACPI MCFG at' "$$log" || \
				diagnostics_ok=false ;; \
		pci-ecam) \
			grep -Fq 'Sapote: ACPI MCFG at' "$$log" && \
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
		device-windows) \
			grep -Eq '^ST DEVICE-WINDOWS WINDOWS [1-9][0-9]* PAGES [1-9][0-9]* VGA 1 LOCAL-APIC 1 IO-APICS [1-9][0-9]* ECAM 1 FRAMEBUFFER 1$$' "$$log" || \
				diagnostics_ok=false ;; \
		boot-ledger) \
			grep -Eq '^ST LEDGER stages [1-9][0-9]* receipts [1-9][0-9]* capabilities [1-9][0-9]* skips [0-9]+ fingerprint 0x[0-9A-F]{16}$$' "$$log" && \
			grep -Fxq 'Sapote: Boot Ledger installed proof passed' "$$log" || \
				diagnostics_ok=false ;; \
		first-light) \
			grep -Eq '^ST FIRST_LIGHT geometry 1024x768 dock 4 events [1-9][0-9]* panels [4-9][0-9]* cursor [1-9][0-9]* damage [1-9][0-9]* glyphs [1-9][0-9]* fingerprint 0x[0-9A-F]{16}$$' "$$log" && \
			grep -Fxq 'Sapote: First Light installed proof passed' "$$log" || \
				diagnostics_ok=false ;; \
		device-substrate) \
			grep -Fxq 'ST DEVICE_SUBSTRATE dma 64 msix 1 used 0->1 ownership CPU-DEVICE-CPU teardown clean negatives 14' "$$log" && \
			grep -Fxq 'Sapote: device substrate teardown complete' "$$log" && \
			grep -Eq '^Sapote: VirtIO RNG device DMA wrote 64 bytes; nonzero [1-9][0-9]*$$' "$$log" && \
			grep -Fxq 'Sapote: MSI-X delivered 1 interrupt; used ring 0 -> 1' "$$log" || \
				diagnostics_ok=false ;; \
		xhci) \
			grep -Fxq 'ST XHCI descriptor 18 msix 1 ownership CPU-CONTROLLER-CPU teardown clean robustness 19' "$$log" && \
			grep -Fxq 'Sapote: xHCI controller ready' "$$log" && \
			grep -Fxq 'Sapote: USB device descriptor DMA completed: 18 bytes' "$$log" && \
			grep -Fxq 'Sapote: xHCI MSI-X descriptor completion count 1' "$$log" && \
			grep -Fxq 'Sapote: xHCI DMA ownership CPU-CONTROLLER-CPU complete' "$$log" && \
			grep -Fxq 'Sapote: xHCI teardown complete' "$$log" || \
				diagnostics_ok=false ;; \
		nvme) \
			grep -Fxq 'ST NVME read 4096 msix 1 ownership CPU-CONTROLLER-CPU teardown clean robustness 22' "$$log" && \
			grep -Fxq 'Sapote: NVMe controller ready' "$$log" && \
			grep -Fxq 'Sapote: NVMe namespace ready' "$$log" && \
			grep -Fxq 'Sapote: NVMe block read completed: 4096 bytes' "$$log" && \
			grep -Fxq 'Sapote: NVMe MSI-X read completion count 1' "$$log" && \
			grep -Fxq 'Sapote: NVMe DMA ownership CPU-CONTROLLER-CPU complete' "$$log" && \
				grep -Fxq 'Sapote: NVMe teardown complete' "$$log" || \
				diagnostics_ok=false ;; \
		filesystem) \
			grep -Fxq 'ST FAT16 file SAPOTE.BIN bytes 128 reads 4 msix 4 ownership CPU-CONTROLLER-CPU teardown clean robustness 28' "$$log" && \
			grep -Fxq 'Sapote: NVMe fixture absent' "$$log" && \
			grep -Fxq 'Sapote: FAT16 volume ready' "$$log" && \
			grep -Fxq 'Sapote: FAT16 file SAPOTE.BIN read: 128 bytes' "$$log" && \
			grep -Fxq 'Sapote: FAT16 MSI-X completion count 4' "$$log" && \
			grep -Fxq 'Sapote: FAT16 DMA ownership CPU-CONTROLLER-CPU complete' "$$log" && \
			grep -Fxq 'Sapote: FAT16 teardown complete' "$$log" || \
				diagnostics_ok=false ;; \
		process) \
			grep -Fxq 'ST PROCESS ELF64 SAPOTE.BIN bytes 128 segments 1 ring 3 address-space private result valid teardown clean robustness 50' "$$log" && \
			grep -Fxq 'Sapote: NVMe fixture absent' "$$log" && \
			grep -Fxq 'Sapote: FAT16 fixture absent' "$$log" && \
			grep -Fxq 'Sapote: process address-space foundation controls 8/8 passed' "$$log" && \
			grep -Fxq 'Sapote: ELF64 parser robustness controls 34/34 passed' "$$log" || \
				diagnostics_ok=false ;; \
		linux-abi) \
			grep -Fxq 'ST LINUX ABI busybox echo bytes 7 syscalls 9 stdout valid exit 0 ring 3 address-space private teardown clean robustness 72' "$$log" && \
			grep -Fxq 'Sapote: Linux SYSCALL CPU foundation controls 10/10 passed' "$$log" && \
			grep -Fxq 'Sapote: BusyBox image and Linux stack controls 32/32 passed' "$$log" && \
			grep -Fqx 'SAPOTE' "$$log" || \
				diagnostics_ok=false ;; \
		linux-abi-uname) \
			grep -Fxq 'ST LINUX ABI busybox uname bytes 6 syscalls 6 output valid exit 0 ring 3 address-space private copy-out valid teardown clean robustness 97' "$$log" && \
			grep -Fxq 'Sapote: Linux SYSCALL CPU foundation controls 10/10 passed' "$$log" && \
			grep -Fxq 'Sapote: BusyBox uname image and UTS controls 50/50 passed' "$$log" && \
			grep -Fqx 'Linux' "$$log" || \
				diagnostics_ok=false ;; \
		first-light-userland) \
			grep -Fxq 'ST FIRST_LIGHT_USERLAND shell production echo 2 uname 2 invalid-profile recovered CPL3 SYSCALL stdout exact exit 0 teardown clean prompt restored' "$$log" && \
			test "$$(grep -Fxc 'SAPOTE' "$$log")" -eq 2 && \
			test "$$(grep -Fxc 'Linux' "$$log")" -eq 2 && \
			grep -Fxq 'FL USERLAND launch completed successfully echo ordinal 2' "$$log" && \
			grep -Fxq 'FL USERLAND launch completed successfully uname ordinal 2' "$$log" && \
			grep -Fxq 'FL USERLAND First Light prompt restored' "$$log" || \
				diagnostics_ok=false ;; \
		first-light-userland-absent) \
			grep -Fxq 'linux: userspace volume unavailable' "$$log" && \
			grep -Fxq 'still usable' "$$log" && \
			grep -Fxq 'ST FIRST_LIGHT_USERLAND_ABSENT concise refusal prompt usable teardown clean' "$$log" && \
			grep -Fxq 'FL USERLAND launch refused and teardown complete' "$$log" && \
			grep -Fxq 'FL USERLAND First Light prompt restored' "$$log" || \
				diagnostics_ok=false ;; \
		first-light-userland-interactive) \
			grep -Fxq 'ST FIRST_LIGHT_USERLAND_INTERACTIVE cat 2 keyboard IRQ read SYSCALL copy-out resume write SYSCALL stdout exact EOF exit 0 teardown clean fresh generation prompt restored' "$$log" && \
			test "$$(grep -Fxc 'FL USERLAND command accepted through First Light shell linux cat' "$$log")" -eq 2 && \
			test "$$(grep -Fxc 'FL USERLAND deterministic read-only NVMe/FAT16 profile selected cat CATBOX' "$$log")" -eq 2 && \
			test "$$(grep -Fxc 'FL USERLAND Rust FAT16 SHA-256 ELF64 validation passed cat bytes 38632' "$$log")" -eq 2 && \
			test "$$(grep -Fxc 'FL USERLAND private CPL3 address space entered cat' "$$log")" -eq 2 && \
			test "$$(grep -Fxc 'FL CAT authentic read SYSCALL observed' "$$log")" -eq 4 && \
			test "$$(grep -Fxc 'FL CAT suspended with kernel CR3 and safe stack restored' "$$log")" -eq 4 && \
			grep -Fxq 'FL CAT terminal input accepted through keyboard events bytes 7' "$$log" && \
			grep -Fxq 'FL CAT terminal input accepted through keyboard events bytes 6' "$$log" && \
			test "$$(grep -Fxc 'FL CAT destination validated and all-or-nothing copy-out complete' "$$log")" -eq 2 && \
			test "$$(grep -Fxc 'FL CAT runtime negative controls 28/28 passed' "$$log")" -eq 2 && \
			test "$$(grep -Fxc 'FL CAT authenticated process generation ready to resume' "$$log")" -eq 4 && \
			test "$$(grep -Fxc 'FL CAT authentic write SYSCALL observed' "$$log")" -eq 2 && \
			test "$$(grep -Fxc 'FL CAT userspace stdout accepted' "$$log")" -eq 2 && \
			test "$$(grep -Fxc 'FL CAT EOF converted to zero-length read result' "$$log")" -eq 2 && \
			test "$$(grep -Fxc 'FL CAT exit status zero observed' "$$log")" -eq 2 && \
			test "$$(grep -Fxc 'FL CAT address-space teardown complete' "$$log")" -eq 2 && \
			test "$$(grep -Fxc 'FL USERLAND First Light prompt restored' "$$log")" -eq 2 && \
			test "$$(grep -Fxc 'pebble' "$$log")" -eq 2 && \
			test "$$(grep -Fxc 'again' "$$log")" -eq 2 && \
			grep -Fxq 'FL USERLAND launch completed successfully cat ordinal 2' "$$log" || \
				diagnostics_ok=false ;; \
		first-light-userland-interactive-absent) \
			grep -Fxq 'ST FIRST_LIGHT_USERLAND_INTERACTIVE_ABSENT cat missing echo valid keyboard IRQ refusal recoverable teardown clean prompt usable' "$$log" && \
			grep -Fxq 'linux: measured profile refused' "$$log" && \
			grep -Fxq 'FL USERLAND deterministic read-only NVMe/FAT16 profile selected cat CATBOX' "$$log" && \
			grep -Fxq 'FL USERLAND launch refused and teardown complete' "$$log" && \
			grep -Fxq 'FL USERLAND command accepted through First Light shell linux echo' "$$log" && \
			grep -Fqx 'SAPOTE' "$$log" && \
			grep -Fxq 'FL USERLAND launch completed successfully echo ordinal 1' "$$log" && \
			test "$$(grep -Fxc 'FL USERLAND First Light prompt restored' "$$log")" -eq 2 || \
				diagnostics_ok=false ;; \
		fat32-system) \
			grep -Fxq 'ST FAT32 SYSTEM authenticated echo uname FAT32 immutable' "$$log" && \
			grep -Fxq 'FL USERLAND deterministic read-only NVMe/FAT32 profile selected echo BUSYBOX' "$$log" && \
			grep -Fxq 'FL USERLAND deterministic read-only NVMe/FAT32 profile selected uname UNAMEBOX' "$$log" && \
			grep -Fxq 'FL USERLAND Rust FAT32 SHA-256 ELF64 validation passed echo bytes 33584' "$$log" && \
			grep -Fxq 'FL USERLAND Rust FAT32 SHA-256 ELF64 validation passed uname bytes 38368' "$$log" || \
				diagnostics_ok=false ;; \
		fat32-data) \
			grep -Fxq 'ST FAT32 DATA create read write append sync exact' "$$log" && \
			grep -Fxq 'data synchronized' "$$log" || diagnostics_ok=false ;; \
		fat32-nested) \
			grep -Fxq 'ST FAT32 NESTED dot dotdot traversal enumeration exact' "$$log" || diagnostics_ok=false ;; \
		fat32-growth) \
			grep -Fxq 'ST FAT32 GROWTH bytes 568 clusters 2 contents readable' "$$log" || diagnostics_ok=false ;; \
		fat32-random) \
			grep -Fxq 'ST FAT32 RANDOM seek overwrite preserved surrounding bytes' "$$log" || diagnostics_ok=false ;; \
		fat32-truncate) \
			grep -Fxq 'ST FAT32 TRUNCATE release regrow zero tail exact' "$$log" || diagnostics_ok=false ;; \
		fat32-rename) \
			grep -Fxq 'ST FAT32 RENAME file move directory parent updated' "$$log" || diagnostics_ok=false ;; \
		fat32-delete) \
			grep -Fxq 'ST FAT32 DELETE cluster reused nonempty directory refused' "$$log" || diagnostics_ok=false ;; \
		fat32-full) \
			grep -Fxq 'ST FAT32 FULL refusal no leak deletion recovered' "$$log" && \
			grep -Fxq 'write: volume has no free cluster' "$$log" || diagnostics_ok=false ;; \
		fat32-corrupt) \
			grep -Fxq 'ST FAT32 CORRUPT refused session usable system executable valid' "$$log" && \
			grep -Fxq 'data    fat32  unavailable' "$$log" && \
			grep -Fqx 'SAPOTE' "$$log" || diagnostics_ok=false ;; \
		fat32-missing) \
			grep -Fxq 'ST FAT32 MISSING session usable system executable valid' "$$log" && \
			grep -Fxq 'data    fat32  absent' "$$log" && \
			grep -Fqx 'SAPOTE' "$$log" || diagnostics_ok=false ;; \
		fat32-persistence) \
			grep -Fxq 'ST FAT32 PERSISTENCE synchronized reboot phase' "$$log" && \
			grep -Fxq 'ST FAT32 PERSISTENCE clean reboot retained exact contents' "$$log" && \
			grep -Fqx 'first cut' "$$log" && grep -Fqx 'second line' "$$log" || \
				diagnostics_ok=false ;; \
		fat32-cache) \
			grep -Fxq 'ST FAT32 CACHE six clusters eviction sync readback exact' "$$log" || diagnostics_ok=false ;; \
		fat32-immutable) \
			grep -Fxq 'ST FAT32 IMMUTABLE write refused below shell executable valid' "$$log" && \
			grep -Fqx 'SAPOTE' "$$log" || diagnostics_ok=false ;; \
		fat32-handles) \
			grep -Fxq 'ST FAT32 HANDLES generation stale double-close access bound clean' "$$log" || diagnostics_ok=false ;; \
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

run: iso $(FAT32_SYSTEM_IMAGE) $(FAT32_DATA_IMAGE)
	cp $(FAT32_DATA_IMAGE) $(FAT32_RUN_DATA_IMAGE)
	qemu-system-x86_64 -m 128M -smp 1 -boot order=d -cdrom $(ISO) \
		-blockdev driver=file,filename=$(FAT32_SYSTEM_IMAGE),node-name=system-file,read-only=on,auto-read-only=off \
		-blockdev driver=raw,file=system-file,node-name=system-raw,read-only=on \
		-device nvme,serial=sapote-system-fat32,drive=system-raw,logical_block_size=512,physical_block_size=512,max_ioqpairs=1,msix_qsize=1 \
		-blockdev driver=file,filename=$(FAT32_RUN_DATA_IMAGE),node-name=data-file,read-only=off,auto-read-only=off \
		-blockdev driver=raw,file=data-file,node-name=data-raw,read-only=off \
		-device nvme,serial=sapote-data-fat32,drive=data-raw,logical_block_size=512,physical_block_size=512,max_ioqpairs=1,msix_qsize=1 \
		-serial stdio -no-reboot -no-shutdown

hooks:
	git config core.hooksPath .githooks
	@echo "repository hooks enabled"

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPENDENCIES)
