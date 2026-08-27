/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Five bounded drivers for the register contracts an NVIDIA board publishes.
 *
 * Sapote has thirteen bounded drivers already, and every one of them was
 * written the same way: read the registers the vendor's own document says
 * carry the device's identity, check what came back against a property that
 * document guarantees, and refuse the device rather than report whatever was
 * found. NVIDIA publishes no such document for its graphics parts. What exists
 * instead is a body of public material that is, in aggregate, better evidence
 * than a datasheet: the envytools project's reverse-engineered register
 * database, the Nouveau driver that has run on this silicon in the Linux tree
 * for fifteen years, Mesa's NVK back end, and NVIDIA's own open GPU kernel
 * modules and published headers. Every offset and every rule below comes from
 * that material, and the comment above each one says which part of it.
 *
 * The honest limit, stated once here and again in docs/NVIDIA.md: none of this
 * has touched NVIDIA silicon. No such device was reachable from the machine
 * this was written on, and QEMU models none, so the bind path is code that has
 * never run against the hardware it describes. What is proved on every boot is
 * everything that does not need the device -- the identity decode against the
 * published encoding, the VBIOS parser against a reference image pinned three
 * independent ways, the refusal of every function that is not NVIDIA's, and a
 * resource census that is identical afterwards.
 *
 * No driver here enables bus mastering or allocates DMA, so none of them can
 * reach memory: without an IOMMU that is the difference between a driver that
 * cannot corrupt the kernel and one that is merely not expected to. Exactly
 * one of them writes a register -- the ROM shadow-disable bit the PROM window
 * requires -- and it restores what it found and reads it back to prove it.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sapote/clock.h>
#include <sapote/cpu.h>
#include <sapote/dma.h>
#include <sapote/interrupt_vector.h>
#include <sapote/memory.h>
#include <sapote/msix.h>
#include <sapote/nvidia.h>
#include <sapote/paging.h>
#include <sapote/pci.h>
#include <sapote/pci_resource.h>

/* PCI Code and ID Assignment Specification 1.19 section 1. */
#define NVIDIA_CLASS_DISPLAY UINT8_C(0x03)
#define NVIDIA_CLASS_MULTIMEDIA UINT8_C(0x04)
#define NVIDIA_SUBCLASS_VGA UINT8_C(0x00)
#define NVIDIA_SUBCLASS_3D UINT8_C(0x02)
#define NVIDIA_SUBCLASS_HD_AUDIO UINT8_C(0x03)
#define NVIDIA_MATCH_ANY UINT8_C(0xFF)

/*
 * envytools, hw/pmc.txt: the master control area occupies the first four
 * kilobytes of the register aperture on every part since NV3, and BOOT_0 at
 * offset zero is the first register any driver for this hardware reads.
 */
#define NV_PMC_BOOT_0 UINT64_C(0x000000)
#define NV_PMC_BOOT_1 UINT64_C(0x000004)

/*
 * Nouveau, nvkm_device_ctor: the part number and its revision are both fields
 * of BOOT_0, and the whole device table is keyed off them.
 */
#define NV_PMC_BOOT_0_CHIPSET_MASK UINT32_C(0x1FF00000)
#define NV_PMC_BOOT_0_CHIPSET_SHIFT 20U
#define NV_PMC_BOOT_0_REVISION_MASK UINT32_C(0x000000FF)
#define NVIDIA_CHIPSET_MASK UINT32_C(0x1FF)
#define NVIDIA_FAMILY_MASK UINT32_C(0x1F0)
#define NVIDIA_IMPLEMENTATION_MASK UINT32_C(0x00F)

/*
 * envytools, hw/pmc.txt: BOOT_1 is the endian switch. A part answering
 * big-endian through a little-endian mapping is not a part this kernel can
 * read, so it is refused rather than misread.
 */
#define NV_PMC_BOOT_1_ENDIAN_BIG UINT32_C(0x01000001)

/*
 * Nouveau, nvkm/subdev/pci: the PCI configuration space of the graphics
 * function is mirrored into the register aperture, at 0x001800 on parts before
 * NV50 and 0x088000 from NV50 onwards. That mirror is the strongest identity
 * check available here, because it makes the device state its vendor and part
 * a second time through a completely different path from the configuration
 * cycles that enumerated it.
 */
#define NV_PBUS_PCI_MIRROR_LEGACY UINT64_C(0x001800)
#define NV_PBUS_PCI_MIRROR_MODERN UINT64_C(0x088000)

/*
 * envytools, hw/ptimer.txt: PTIMER's count is two registers, and Nouveau's
 * nv04_timer_read reads the high half, then the low half, then the high half
 * again, because the count can carry between the two reads. A timer that never
 * moves is a device that is not running, which is worth knowing and is the one
 * thing in this file that observes the device over time.
 */
#define NV_PTIMER_TIME_0 UINT64_C(0x009400)
#define NV_PTIMER_TIME_1 UINT64_C(0x009410)

/*
 * Nouveau, nvkm/subdev/bios/shadowrom.c: the video BIOS is readable through a
 * window at 0x300000 once the ROM shadow is switched off, and the bit that
 * switches it off lives in the configuration mirror at offset 0x50. Clearing
 * it exposes the real ROM; the driver puts back exactly what it found.
 */
#define NV_PROM_BASE UINT64_C(0x300000)
#define NV_PBUS_PCI_NV_20_OFFSET UINT64_C(0x50)
#define NV_PBUS_PCI_NV_20_ROM_SHADOW UINT32_C(0x00000001)

/*
 * How much of the PROM window is read. A legal image declares its own length,
 * and the structures this kernel reads -- the expansion ROM header, the PCIR
 * data structure and the BIT table -- are near the front of every image the
 * public tooling describes. Eight kilobytes is a bound, not a belief: an image
 * whose BIT table is beyond it is reported as malformed rather than searched
 * for further.
 */
#define NVIDIA_VBIOS_READ_BYTES 8192U

_Static_assert(NVIDIA_VBIOS_READ_BYTES <= NVIDIA_VBIOS_MAX_BYTES,
    "the video BIOS prefix is larger than the PROM aperture");
_Static_assert(NVIDIA_VBIOS_READ_BYTES % NVIDIA_VBIOS_BLOCK_BYTES == 0U,
    "the video BIOS prefix is not a whole number of ROM blocks");

/* High Definition Audio Specification 1.0a sections 3.3.1 through 3.3.3. */
#define HDA_GCAP UINT64_C(0x00)
#define HDA_VMIN UINT64_C(0x02)
#define HDA_VMAJ UINT64_C(0x03)
#define HDA_GCAP_OUTPUT_STREAM_SHIFT 12U
#define HDA_GCAP_OUTPUT_STREAM_MASK UINT16_C(0x000F)
#define HDA_VERSION_MAJOR UINT8_C(1)
#define HDA_VERSION_MINOR UINT8_C(0)

/*
 * The one status value that crosses back from the Rust validator by number.
 * `nvbios::Status` is `#[repr(i32)]` with explicit discriminants, and a window
 * with no ROM in it fails at the signature rather than anywhere later, which
 * is the difference between "there is no video BIOS here" and "there is one
 * and it is wrong".
 */
#define NVBIOS_STATUS_SIGNATURE 2

/* Where the C copy of the reference image and Rust's must agree. */
#define NVIDIA_REFERENCE_VBIOS_BYTES 1024U
#define NVIDIA_REFERENCE_VBIOS_DEVICE UINT16_C(0x5341)
#define NVIDIA_VBIOS_ROBUSTNESS_CONTROLS 16U

struct nvidia_driver_record;

typedef enum nvidia_status (*nvidia_probe_t)(
    const struct nvidia_driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct nvidia_driver_probe *probe
);

struct nvidia_driver_record {
    const char *name;
    uint8_t class_code;
    /* NVIDIA_MATCH_ANY accepts either display subclass. */
    uint8_t subclass;
    uint8_t bar_index;
    uint32_t minimum_register_bytes;
    bool writes_registers;
    nvidia_probe_t probe;
};

struct nvidia_census {
    struct frame_allocator_stats frames;
    struct paging_state paging;
    struct dma_state dma;
    struct pci_resource_state pci;
    struct interrupt_vector_state vectors;
    struct msix_state msix;
    bool interrupts_enabled;
};

/* The freestanding Rust validator; C never parses a VBIOS byte itself. */
extern uint32_t sapote_nvbios_self_test(void);
extern uint32_t sapote_nvbios_controls(void);
extern size_t sapote_nvbios_reference(uint8_t *out, size_t capacity);
extern int sapote_nvbios_parse(
    const uint8_t *input,
    size_t input_len,
    struct nvidia_vbios_image *out
);

static struct nvidia_result installed_result;
static bool nvidia_active;
static uint32_t register_reads;
static uint32_t register_writes;
/*
 * Drivers one and three need to know which side of the NV50 boundary the part
 * is on to pick their register offsets, and driver zero is what establishes
 * that. The table is ordered so the answer exists before it is needed, and a
 * part whose identity was never established uses the modern offsets and says
 * so through its status rather than guessing silently.
 */
static struct nvidia_identity current_identity;
static uint8_t vbios_window[NVIDIA_VBIOS_READ_BYTES];

/*
 * The reference VBIOS image, stated here in C, in freestanding Rust, and in a
 * Python record the build compares against this table. It is synthesised
 * rather than dumped: no board's ROM is reproduced here, and its PCIR device
 * identifier is 0x5341 so it can never be mistaken for a real part.
 */
static const uint8_t reference_vbios[NVIDIA_REFERENCE_VBIOS_BYTES] = {
    0x55, 0xAA, 0x02, 0xEB, 0x0A, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x50, 0x43, 0x49, 0x52, 0xDE, 0x10, 0x41, 0x53,
    0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x03, 0x02, 0x00, 0x00, 0x00,
    0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xFF, 0xB8, 0x42, 0x49, 0x54, 0x00, 0x01, 0x00,
    0x0C, 0x06, 0x03, 0x00, 0x69, 0x02, 0x40, 0x00, 0x00, 0x02, 0x42, 0x02,
    0x20, 0x00, 0x40, 0x02, 0x50, 0x02, 0x10, 0x00, 0x60, 0x02, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};

_Static_assert(sizeof(reference_vbios) == NVIDIA_REFERENCE_VBIOS_BYTES,
    "the reference VBIOS table is not the declared length");

static void zero_bytes(void *pointer, size_t length)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static uint8_t mmio_read8(volatile uint8_t *base, uint64_t offset)
{
    ++register_reads;
    return *(volatile uint8_t *)(void *)(base + offset);
}

static uint16_t mmio_read16(volatile uint8_t *base, uint64_t offset)
{
    ++register_reads;
    return *(volatile uint16_t *)(void *)(base + offset);
}

static uint32_t mmio_read32(volatile uint8_t *base, uint64_t offset)
{
    ++register_reads;
    return *(volatile uint32_t *)(void *)(base + offset);
}

static void mmio_write32(
    volatile uint8_t *base,
    uint64_t offset,
    uint32_t value
)
{
    ++register_writes;
    *(volatile uint32_t *)(void *)(base + offset) = value;
}

static bool configuration_dword(
    const struct pci_function *function,
    uint16_t offset,
    uint32_t *value
)
{
    ++register_reads;
    return pci_config_read_port(function->address, offset, value) ==
        PCI_STATUS_OK;
}

/*
 * The whole architecture table, and the only place a family boundary is
 * written down. Nouveau keys its device table off chipset & 0x1f0; these are
 * that table's own boundaries rather than marketing names, which is why a part
 * this kernel has never heard of still lands in the right family.
 */
struct nvidia_family_entry {
    uint32_t family;
    enum nvidia_architecture architecture;
};

static const struct nvidia_family_entry nvidia_families[] = {
    { UINT32_C(0x010), NVIDIA_ARCHITECTURE_CELSIUS },
    { UINT32_C(0x020), NVIDIA_ARCHITECTURE_KELVIN },
    { UINT32_C(0x030), NVIDIA_ARCHITECTURE_RANKINE },
    { UINT32_C(0x040), NVIDIA_ARCHITECTURE_CURIE },
    { UINT32_C(0x060), NVIDIA_ARCHITECTURE_CURIE },
    { UINT32_C(0x050), NVIDIA_ARCHITECTURE_TESLA },
    { UINT32_C(0x080), NVIDIA_ARCHITECTURE_TESLA },
    { UINT32_C(0x090), NVIDIA_ARCHITECTURE_TESLA },
    { UINT32_C(0x0A0), NVIDIA_ARCHITECTURE_TESLA },
    { UINT32_C(0x0C0), NVIDIA_ARCHITECTURE_FERMI },
    { UINT32_C(0x0D0), NVIDIA_ARCHITECTURE_FERMI },
    { UINT32_C(0x0E0), NVIDIA_ARCHITECTURE_KEPLER },
    { UINT32_C(0x0F0), NVIDIA_ARCHITECTURE_KEPLER },
    { UINT32_C(0x100), NVIDIA_ARCHITECTURE_KEPLER },
    { UINT32_C(0x110), NVIDIA_ARCHITECTURE_MAXWELL },
    { UINT32_C(0x120), NVIDIA_ARCHITECTURE_MAXWELL },
    { UINT32_C(0x130), NVIDIA_ARCHITECTURE_PASCAL },
    { UINT32_C(0x140), NVIDIA_ARCHITECTURE_VOLTA },
    { UINT32_C(0x160), NVIDIA_ARCHITECTURE_TURING },
    { UINT32_C(0x170), NVIDIA_ARCHITECTURE_AMPERE },
    { UINT32_C(0x190), NVIDIA_ARCHITECTURE_ADA }
};

#define NVIDIA_FAMILY_COUNT \
    (sizeof(nvidia_families) / sizeof(nvidia_families[0]))

struct nvidia_identity nvidia_decode_identity(uint32_t boot0)
{
    struct nvidia_identity identity;

    zero_bytes(&identity, sizeof(identity));
    identity.boot0 = boot0;
    identity.chipset = (boot0 & NV_PMC_BOOT_0_CHIPSET_MASK) >>
        NV_PMC_BOOT_0_CHIPSET_SHIFT;
    identity.revision = boot0 & NV_PMC_BOOT_0_REVISION_MASK;
    identity.family = identity.chipset & NVIDIA_FAMILY_MASK;
    identity.implementation = identity.chipset & NVIDIA_IMPLEMENTATION_MASK;
    identity.architecture = NVIDIA_ARCHITECTURE_UNKNOWN;
    for (size_t index = 0U; index < NVIDIA_FAMILY_COUNT; ++index) {
        if (nvidia_families[index].family == identity.family) {
            identity.architecture = nvidia_families[index].architecture;
            break;
        }
    }
    /*
     * A bus that answers with all ones decodes to chipset 0x1ff, and an
     * aperture that is not there at all decodes to zero. Neither is a family,
     * so neither is recognized, which is what keeps a missing device from
     * being reported as an ancient one.
     */
    identity.recognized =
        identity.architecture != NVIDIA_ARCHITECTURE_UNKNOWN &&
        boot0 != 0U && boot0 != UINT32_MAX;
    return identity;
}

const char *nvidia_architecture_name(enum nvidia_architecture architecture)
{
    static const char *const names[NVIDIA_ARCHITECTURE_COUNT] = {
        "unknown", "Celsius", "Kelvin", "Rankine", "Curie", "Tesla",
        "Fermi", "Kepler", "Maxwell", "Pascal", "Volta", "Turing",
        "Ampere", "Ada"
    };

    _Static_assert(sizeof(names) / sizeof(names[0]) ==
        NVIDIA_ARCHITECTURE_COUNT, "NVIDIA architecture names drifted");
    if (architecture < NVIDIA_ARCHITECTURE_UNKNOWN ||
        architecture >= NVIDIA_ARCHITECTURE_COUNT) {
        return "unknown";
    }
    return names[architecture];
}

static bool identity_is_modern(void)
{
    /*
     * The configuration mirror and the ROM shadow bit both moved at NV50.
     * Anything Tesla or newer uses the modern offsets; an unrecognized part
     * uses them too, because every part made this century is on that side.
     */
    return !current_identity.recognized ||
        current_identity.architecture >= NVIDIA_ARCHITECTURE_TESLA;
}

static uint64_t configuration_mirror_base(void)
{
    return identity_is_modern() ? NV_PBUS_PCI_MIRROR_MODERN :
        NV_PBUS_PCI_MIRROR_LEGACY;
}

const uint8_t *nvidia_reference_vbios(size_t *length)
{
    if (length == NULL) {
        return NULL;
    }
    *length = NVIDIA_REFERENCE_VBIOS_BYTES;
    return reference_vbios;
}

/*
 * Driver zero. The first register on the part, and the one every driver for
 * this hardware has read first since 1999. What it carries is the part number
 * and its revision; what its neighbour carries is the endianness the register
 * aperture is answering in. A part that answers big-endian is refused rather
 * than read backwards.
 */
static enum nvidia_status probe_master_control(
    const struct nvidia_driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct nvidia_driver_probe *probe
)
{
    uint32_t boot0;
    uint32_t boot1;
    struct nvidia_identity identity;

    (void)record;
    (void)function;
    (void)claim;
    if (register_bytes < NV_PMC_BOOT_1 + 4U) {
        return NVIDIA_STATUS_REGISTER_WINDOW;
    }
    boot0 = mmio_read32(registers, NV_PMC_BOOT_0);
    boot1 = mmio_read32(registers, NV_PMC_BOOT_1);
    identity = nvidia_decode_identity(boot0);
    probe->identity = boot0;
    probe->detail = boot1;
    if (!identity.recognized) {
        return NVIDIA_STATUS_IDENTITY;
    }
    if ((boot1 & NV_PMC_BOOT_1_ENDIAN_BIG) != 0U) {
        return NVIDIA_STATUS_ENDIANNESS;
    }
    current_identity = identity;
    installed_result.identity = identity;
    return NVIDIA_STATUS_OK;
}

/*
 * Driver one. The graphics function mirrors its own PCI configuration space
 * into the register aperture, so the first dword of that mirror has to be the
 * same vendor and device the enumeration read through configuration cycles.
 * One source could be anything. Two sources that agree, reached through
 * completely different hardware paths, are the device telling the truth about
 * itself twice.
 */
static enum nvidia_status probe_configuration_mirror(
    const struct nvidia_driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct nvidia_driver_probe *probe
)
{
    const uint64_t base = configuration_mirror_base();
    uint32_t mirrored;
    uint32_t enumerated = 0U;

    (void)record;
    (void)claim;
    if (register_bytes < base + 4U) {
        return NVIDIA_STATUS_REGISTER_WINDOW;
    }
    mirrored = mmio_read32(registers, base);
    if (!configuration_dword(function, 0U, &enumerated)) {
        return NVIDIA_STATUS_IDENTITY;
    }
    probe->identity = mirrored;
    probe->detail = enumerated;
    if ((mirrored & UINT32_C(0xFFFF)) != NVIDIA_VENDOR_ID ||
        mirrored != enumerated) {
        return NVIDIA_STATUS_IDENTITY;
    }
    return NVIDIA_STATUS_OK;
}

/*
 * Driver two. PTIMER's count is wider than a register, so it is read high,
 * low, high again and reassembled only if the high half did not move; that is
 * Nouveau's own sequence and it is the only correct way to read a counter that
 * can carry underneath you. A count that has not advanced after a bounded wait
 * on this kernel's own clock is a stopped timer, reported as one.
 */
static bool read_ptimer(volatile uint8_t *registers, uint64_t *value)
{
    for (unsigned attempt = 0U; attempt < 4U; ++attempt) {
        const uint32_t high = mmio_read32(registers, NV_PTIMER_TIME_1);
        const uint32_t low = mmio_read32(registers, NV_PTIMER_TIME_0);

        if (mmio_read32(registers, NV_PTIMER_TIME_1) == high) {
            *value = ((uint64_t)high << 32U) | low;
            return true;
        }
    }
    return false;
}

static enum nvidia_status probe_timer(
    const struct nvidia_driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct nvidia_driver_probe *probe
)
{
    uint64_t first = 0U;
    uint64_t second = 0U;
    uint64_t deadline;

    (void)record;
    (void)function;
    (void)claim;
    if (register_bytes < NV_PTIMER_TIME_1 + 4U) {
        return NVIDIA_STATUS_REGISTER_WINDOW;
    }
    if (!read_ptimer(registers, &first)) {
        return NVIDIA_STATUS_TIMER_STOPPED;
    }
    deadline = clock_monotonic_ns() + NVIDIA_TIMER_OBSERVATION_NS;
    while (clock_monotonic_ns() < deadline) {
        __asm__ volatile ("" : : : "memory");
    }
    if (!read_ptimer(registers, &second)) {
        return NVIDIA_STATUS_TIMER_STOPPED;
    }
    probe->identity = second;
    probe->detail = second - first;
    if (second <= first) {
        return NVIDIA_STATUS_TIMER_STOPPED;
    }
    return NVIDIA_STATUS_OK;
}

/*
 * Driver three. The only driver here that writes anything, and the write is
 * the one the window requires: clearing the ROM shadow bit in the
 * configuration mirror is what makes the PROM aperture answer with the real
 * image instead of the shadow copy. The original value goes back afterwards
 * and is read again to prove it went back, because a driver that leaves a
 * device in a state it did not find it in has not finished.
 *
 * The bytes themselves are never parsed here. They go straight to the
 * freestanding Rust validator, which is the whole reason that crate exists.
 */
static enum nvidia_status probe_video_bios(
    const struct nvidia_driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct nvidia_driver_probe *probe
)
{
    const uint64_t shadow = configuration_mirror_base() +
        NV_PBUS_PCI_NV_20_OFFSET;
    uint32_t saved;
    uint32_t restored;
    struct nvidia_vbios_image image;
    int status;

    (void)record;
    (void)function;
    (void)claim;
    if (register_bytes < NV_PROM_BASE + NVIDIA_VBIOS_READ_BYTES ||
        register_bytes < shadow + 4U) {
        return NVIDIA_STATUS_REGISTER_WINDOW;
    }
    saved = mmio_read32(registers, shadow);
    mmio_write32(registers, shadow, saved & ~NV_PBUS_PCI_NV_20_ROM_SHADOW);
    for (size_t offset = 0U; offset < NVIDIA_VBIOS_READ_BYTES; ++offset) {
        vbios_window[offset] = mmio_read8(registers, NV_PROM_BASE + offset);
    }
    mmio_write32(registers, shadow, saved);
    restored = mmio_read32(registers, shadow);
    if (restored != saved) {
        return NVIDIA_STATUS_ROM_NOT_RESTORED;
    }
    zero_bytes(&image, sizeof(image));
    status = sapote_nvbios_parse(vbios_window, sizeof(vbios_window), &image);
    probe->identity = ((uint64_t)image.vendor_id << 16U) | image.device_id;
    probe->detail = image.image_bytes;
    if (status != 0) {
        return status == NVBIOS_STATUS_SIGNATURE ? NVIDIA_STATUS_ROM_ABSENT :
            NVIDIA_STATUS_ROM_MALFORMED;
    }
    installed_result.vbios = image;
    installed_result.vbios_valid = true;
    return NVIDIA_STATUS_OK;
}

/*
 * Driver four, and the only one that binds a function other than the graphics
 * one. Every NVIDIA board since Fermi carries an HD Audio controller beside
 * the GPU for the audio a display link carries, and it answers the same
 * register contract Sapote's ICH9 driver already proves: a version, and a
 * count of the streams the controller has. This driver resets nothing: the
 * audio function of a board that may be driving a live display is not
 * something to reset blind.
 */
static enum nvidia_status probe_hd_audio(
    const struct nvidia_driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct nvidia_driver_probe *probe
)
{
    uint16_t capability;
    uint8_t major;
    uint8_t minor;

    (void)record;
    (void)function;
    (void)claim;
    if (register_bytes < HDA_VMAJ + 1U) {
        return NVIDIA_STATUS_REGISTER_WINDOW;
    }
    capability = mmio_read16(registers, HDA_GCAP);
    minor = mmio_read8(registers, HDA_VMIN);
    major = mmio_read8(registers, HDA_VMAJ);
    probe->identity = ((uint64_t)major << 8U) | minor;
    probe->detail = capability;
    if (major != HDA_VERSION_MAJOR || minor != HDA_VERSION_MINOR) {
        return NVIDIA_STATUS_VERSION;
    }
    if (((capability >> HDA_GCAP_OUTPUT_STREAM_SHIFT) &
            HDA_GCAP_OUTPUT_STREAM_MASK) == 0U) {
        return NVIDIA_STATUS_IDENTITY;
    }
    return NVIDIA_STATUS_OK;
}

/*
 * The order matters exactly once: driver zero establishes which side of the
 * NV50 boundary the part is on, and drivers one and three pick their register
 * offsets from that.
 */
static const struct nvidia_driver_record nvidia_drivers[NVIDIA_DRIVER_COUNT] = {
    {
        .name = "NVIDIA GPU master control",
        .class_code = NVIDIA_CLASS_DISPLAY,
        .subclass = NVIDIA_MATCH_ANY,
        .bar_index = 0U,
        .minimum_register_bytes = UINT32_C(0x1000),
        .writes_registers = false,
        .probe = probe_master_control
    },
    {
        .name = "NVIDIA GPU configuration mirror",
        .class_code = NVIDIA_CLASS_DISPLAY,
        .subclass = NVIDIA_MATCH_ANY,
        .bar_index = 0U,
        .minimum_register_bytes = UINT32_C(0x89000),
        .writes_registers = false,
        .probe = probe_configuration_mirror
    },
    {
        .name = "NVIDIA GPU timer",
        .class_code = NVIDIA_CLASS_DISPLAY,
        .subclass = NVIDIA_MATCH_ANY,
        .bar_index = 0U,
        .minimum_register_bytes = UINT32_C(0xA000),
        .writes_registers = false,
        .probe = probe_timer
    },
    {
        .name = "NVIDIA GPU video BIOS",
        .class_code = NVIDIA_CLASS_DISPLAY,
        .subclass = NVIDIA_MATCH_ANY,
        .bar_index = 0U,
        .minimum_register_bytes = UINT32_C(0x302000),
        .writes_registers = true,
        .probe = probe_video_bios
    },
    {
        .name = "NVIDIA HD Audio function",
        .class_code = NVIDIA_CLASS_MULTIMEDIA,
        .subclass = NVIDIA_SUBCLASS_HD_AUDIO,
        .bar_index = 0U,
        .minimum_register_bytes = UINT32_C(0x100),
        .writes_registers = false,
        .probe = probe_hd_audio
    }
};

static void capture_census(struct nvidia_census *census)
{
    census->frames = frame_allocator_get_stats();
    census->paging = paging_get_state();
    census->dma = dma_get_state();
    census->pci = pci_resource_get_state();
    census->vectors = interrupt_vector_get_state();
    census->msix = msix_get_state();
    census->interrupts_enabled = cpu_interrupts_enabled();
}

static bool census_equal(
    const struct nvidia_census *left,
    const struct nvidia_census *right
)
{
    return left->frames.free_frames == right->frames.free_frames &&
        left->frames.allocated_frames == right->frames.allocated_frames &&
        left->paging.table_frames == right->paging.table_frames &&
        left->paging.root_physical_address ==
            right->paging.root_physical_address &&
        left->dma.active_allocations == right->dma.active_allocations &&
        left->dma.device_owned_allocations ==
            right->dma.device_owned_allocations &&
        left->pci.active_claims == right->pci.active_claims &&
        left->pci.active_mappings == right->pci.active_mappings &&
        left->pci.mapped_pages == right->pci.mapped_pages &&
        left->pci.bus_masters == right->pci.bus_masters &&
        left->vectors.allocated == right->vectors.allocated &&
        left->vectors.free == right->vectors.free &&
        left->msix.active_bindings == right->msix.active_bindings &&
        left->interrupts_enabled == right->interrupts_enabled;
}

static bool subclass_matches(
    const struct nvidia_driver_record *record,
    const struct pci_function *function
)
{
    if (record->subclass != NVIDIA_MATCH_ANY) {
        return function->subclass == record->subclass;
    }
    /* Boards enumerate as a VGA controller or a bare 3D controller. */
    return function->subclass == NVIDIA_SUBCLASS_VGA ||
        function->subclass == NVIDIA_SUBCLASS_3D;
}

static const struct pci_function *find_function(
    const struct nvidia_driver_record *record
)
{
    for (size_t index = 0U; index < pci_function_count(); ++index) {
        const struct pci_function *function = pci_function_at(index);

        if (function != NULL && function->vendor_id == NVIDIA_VENDOR_ID &&
            function->class_code == record->class_code &&
            subclass_matches(record, function)) {
            return function;
        }
    }
    return NULL;
}

static enum nvidia_status bind_one(
    const struct nvidia_driver_record *record,
    const struct pci_function *function,
    struct nvidia_driver_probe *probe
)
{
    struct pci_device_claim claim;
    struct pci_mmio_region *region = NULL;
    volatile void *pointer = NULL;
    enum nvidia_status status;
    bool mapped = false;
    const uint32_t reads_before = register_reads;
    const uint32_t writes_before = register_writes;

    probe->address = function->address;
    probe->vendor_id = function->vendor_id;
    probe->device_id = function->device_id;
    probe->class_code = function->class_code;
    probe->subclass = function->subclass;
    probe->present = true;

    zero_bytes(&claim, sizeof(claim));
    if (pci_claim_device(function, &claim) != PCI_RESOURCE_STATUS_OK) {
        return NVIDIA_STATUS_CLAIM_FAILURE;
    }
    if (pci_claim_map_bar(&claim, record->bar_index, &region) !=
            PCI_RESOURCE_STATUS_OK || region == NULL) {
        status = NVIDIA_STATUS_MAPPING_FAILURE;
        goto release;
    }
    mapped = true;
    if (region->size < record->minimum_register_bytes ||
        pci_mmio_subregion(region, 0U, region->size, &pointer) !=
            PCI_RESOURCE_STATUS_OK || pointer == NULL) {
        status = NVIDIA_STATUS_REGISTER_WINDOW;
        goto release;
    }
    probe->register_bytes = (uint32_t)region->size;
    status = record->probe(record, function, &claim,
        (volatile uint8_t *)pointer, region->size, probe);

release:
    if (mapped && pci_claim_unmap_last_bar(&claim, record->bar_index) !=
            PCI_RESOURCE_STATUS_OK) {
        status = NVIDIA_STATUS_RELEASE_FAILURE;
    }
    if (pci_release_device(&claim) != PCI_RESOURCE_STATUS_OK) {
        status = NVIDIA_STATUS_RELEASE_FAILURE;
    }
    probe->register_reads = register_reads - reads_before;
    probe->register_writes = register_writes - writes_before;
    probe->bound = status == NVIDIA_STATUS_OK;
    return status;
}

size_t nvidia_driver_count(void)
{
    return NVIDIA_DRIVER_COUNT;
}

const char *nvidia_driver_name(size_t index)
{
    if (index >= NVIDIA_DRIVER_COUNT) {
        return "unknown NVIDIA driver";
    }
    return nvidia_drivers[index].name;
}

uint8_t nvidia_driver_class(size_t index)
{
    if (index >= NVIDIA_DRIVER_COUNT) {
        return UINT8_MAX;
    }
    return nvidia_drivers[index].class_code;
}

uint8_t nvidia_driver_subclass(size_t index)
{
    if (index >= NVIDIA_DRIVER_COUNT) {
        return UINT8_MAX;
    }
    return nvidia_drivers[index].subclass;
}

bool nvidia_driver_writes_registers(size_t index)
{
    if (index >= NVIDIA_DRIVER_COUNT) {
        return false;
    }
    return nvidia_drivers[index].writes_registers;
}

bool nvidia_vbios_image_layout_self_test(void)
{
    return sizeof(struct nvidia_vbios_image) == 24U &&
        offsetof(struct nvidia_vbios_image, image_bytes) == 0U &&
        offsetof(struct nvidia_vbios_image, pcir_offset) == 4U &&
        offsetof(struct nvidia_vbios_image, bit_offset) == 8U &&
        offsetof(struct nvidia_vbios_image, vendor_id) == 12U &&
        offsetof(struct nvidia_vbios_image, device_id) == 14U &&
        offsetof(struct nvidia_vbios_image, class_code) == 16U &&
        offsetof(struct nvidia_vbios_image, subclass) == 17U &&
        offsetof(struct nvidia_vbios_image, programming_interface) == 18U &&
        offsetof(struct nvidia_vbios_image, code_type) == 19U &&
        offsetof(struct nvidia_vbios_image, bit_tokens) == 20U &&
        offsetof(struct nvidia_vbios_image, bit_token_bytes) == 21U &&
        offsetof(struct nvidia_vbios_image, last_image) == 22U;
}

/*
 * The kernel's table and Rust's are two independent statements of the same
 * image, and a Python record the build compares against this table is the
 * third. Any two of them disagreeing is caught here or by `make verify`
 * rather than by a parser that quietly accepts something else.
 */
static bool reference_vbios_agrees(void)
{
    static uint8_t written[NVIDIA_REFERENCE_VBIOS_BYTES];

    if (sapote_nvbios_reference(written, sizeof(written)) !=
            sizeof(written)) {
        return false;
    }
    for (size_t index = 0U; index < sizeof(written); ++index) {
        if (written[index] != reference_vbios[index]) {
            return false;
        }
    }
    return true;
}

/*
 * Everything provable without the device, and therefore everything this
 * increment can actually stand behind. Each control is a statement that would
 * be false if the code were wrong, and none of them needs an NVIDIA part to be
 * present.
 */
bool nvidia_foundation_self_test(size_t *completed_tests)
{
    static const struct { uint32_t boot0; enum nvidia_architecture family; }
        published[] = {
        { UINT32_C(0x050000A2), NVIDIA_ARCHITECTURE_TESLA },
        { UINT32_C(0x0C0000A3), NVIDIA_ARCHITECTURE_FERMI },
        { UINT32_C(0x0E4000A1), NVIDIA_ARCHITECTURE_KEPLER },
        { UINT32_C(0x124000A1), NVIDIA_ARCHITECTURE_MAXWELL },
        { UINT32_C(0x134000A1), NVIDIA_ARCHITECTURE_PASCAL },
        { UINT32_C(0x140000A1), NVIDIA_ARCHITECTURE_VOLTA },
        { UINT32_C(0x164000A1), NVIDIA_ARCHITECTURE_TURING },
        { UINT32_C(0x172000A1), NVIDIA_ARCHITECTURE_AMPERE },
        { UINT32_C(0x192000A1), NVIDIA_ARCHITECTURE_ADA }
    };
    const size_t published_count = sizeof(published) / sizeof(published[0]);
    size_t completed = 0U;
    struct nvidia_vbios_image image;
    size_t reference_length = 0U;
    const uint8_t *reference;

    if (completed_tests == NULL) {
        return false;
    }
    *completed_tests = 0U;

    for (size_t index = 0U; index < NVIDIA_DRIVER_COUNT; ++index) {
        if (nvidia_drivers[index].name == NULL ||
            nvidia_drivers[index].probe == NULL) {
            return false;
        }
    }
    ++completed;

    /* Exactly one driver may write, and it must be the video BIOS one. */
    {
        size_t writers = 0U;

        for (size_t index = 0U; index < NVIDIA_DRIVER_COUNT; ++index) {
            if (nvidia_drivers[index].writes_registers) {
                ++writers;
                if (nvidia_drivers[index].probe != probe_video_bios) {
                    return false;
                }
            }
        }
        if (writers != 1U) {
            return false;
        }
    }
    ++completed;

    /* Every published encoding lands in the family Nouveau's table gives. */
    for (size_t index = 0U; index < published_count; ++index) {
        const struct nvidia_identity identity =
            nvidia_decode_identity(published[index].boot0);

        if (!identity.recognized ||
            identity.architecture != published[index].family ||
            identity.chipset != ((published[index].boot0 >> 20U) &
                NVIDIA_CHIPSET_MASK) ||
            identity.revision != (published[index].boot0 & UINT32_C(0xFF))) {
            return false;
        }
    }
    ++completed;

    /* A family the table does not carry is unknown, never the nearest one. */
    if (nvidia_decode_identity(UINT32_C(0x180000A1)).recognized ||
        nvidia_decode_identity(UINT32_C(0x1A0000A1)).recognized) {
        return false;
    }
    ++completed;

    /* An absent aperture reads as zero and a dead bus reads as all ones. */
    if (nvidia_decode_identity(0U).recognized ||
        nvidia_decode_identity(UINT32_MAX).recognized) {
        return false;
    }
    ++completed;

    /* Every architecture has a distinct name. */
    for (int outer = 0; outer < (int)NVIDIA_ARCHITECTURE_COUNT; ++outer) {
        const char *left = nvidia_architecture_name(
            (enum nvidia_architecture)outer);

        if (left == NULL) {
            return false;
        }
        for (int inner = 0; inner < outer; ++inner) {
            const char *right = nvidia_architecture_name(
                (enum nvidia_architecture)inner);
            size_t position = 0U;

            while (left[position] != '\0' && right[position] != '\0' &&
                left[position] == right[position]) {
                ++position;
            }
            if (left[position] == right[position]) {
                return false;
            }
        }
    }
    ++completed;

    if (!nvidia_vbios_image_layout_self_test()) {
        return false;
    }
    ++completed;

    /* The Rust validator runs every control it declares. */
    if (sapote_nvbios_controls() != NVIDIA_VBIOS_ROBUSTNESS_CONTROLS ||
        sapote_nvbios_self_test() != NVIDIA_VBIOS_ROBUSTNESS_CONTROLS) {
        return false;
    }
    ++completed;

    if (!reference_vbios_agrees()) {
        return false;
    }
    ++completed;

    /* The kernel's copy of the reference image is Rust's, byte for byte. */
    reference = nvidia_reference_vbios(&reference_length);
    if (reference == NULL || reference_length !=
            NVIDIA_REFERENCE_VBIOS_BYTES) {
        return false;
    }
    ++completed;

    /* And it parses through the same boundary a real image would. */
    zero_bytes(&image, sizeof(image));
    if (sapote_nvbios_parse(reference, reference_length, &image) != 0 ||
        image.vendor_id != NVIDIA_VENDOR_ID ||
        image.device_id != NVIDIA_REFERENCE_VBIOS_DEVICE ||
        image.image_bytes != NVIDIA_REFERENCE_VBIOS_BYTES ||
        image.class_code != UINT8_C(0x03) || image.code_type != 0U ||
        image.bit_tokens == 0U || !image.last_image) {
        return false;
    }
    ++completed;

    /* A truncated image is refused rather than read past. */
    zero_bytes(&image, sizeof(image));
    if (sapote_nvbios_parse(reference, 16U, &image) == 0 ||
        image.image_bytes != 0U) {
        return false;
    }
    ++completed;

    /* So is a null one. */
    zero_bytes(&image, sizeof(image));
    if (sapote_nvbios_parse(NULL, reference_length, &image) == 0) {
        return false;
    }
    ++completed;

    /* No two drivers may answer to the same class and subclass. */
    for (size_t index = 0U; index < NVIDIA_DRIVER_COUNT; ++index) {
        for (size_t other = 0U; other < index; ++other) {
            if (nvidia_drivers[index].class_code ==
                    nvidia_drivers[other].class_code &&
                nvidia_drivers[index].subclass ==
                    nvidia_drivers[other].subclass &&
                nvidia_drivers[index].probe ==
                    nvidia_drivers[other].probe) {
                return false;
            }
        }
    }
    ++completed;

    *completed_tests = completed;
    return completed == NVIDIA_CONTROLLED_CONTROLS;
}

enum nvidia_status nvidia_bind(struct nvidia_result *result)
{
    struct nvidia_census before;
    struct nvidia_census after;
    size_t controls = 0U;

    if (result == NULL) {
        return NVIDIA_STATUS_NULL_ARGUMENT;
    }
    if (nvidia_active) {
        return NVIDIA_STATUS_BUSY;
    }
    if (!pci_is_initialized() || !pci_resource_get_state().active) {
        return NVIDIA_STATUS_PREREQUISITE;
    }
    nvidia_active = true;
    zero_bytes(&installed_result, sizeof(installed_result));
    zero_bytes(&current_identity, sizeof(current_identity));
    register_reads = 0U;
    register_writes = 0U;
    installed_result.declared = NVIDIA_DRIVER_COUNT;
    installed_result.failed_driver = NVIDIA_DRIVER_COUNT;
    installed_result.failed_status = NVIDIA_STATUS_OK;

    if (!nvidia_foundation_self_test(&controls) ||
        controls != NVIDIA_CONTROLLED_CONTROLS) {
        installed_result.failed_status = NVIDIA_STATUS_ROBUSTNESS;
        nvidia_active = false;
        *result = installed_result;
        return NVIDIA_STATUS_ROBUSTNESS;
    }
    installed_result.controls = (uint32_t)controls;

    capture_census(&before);
    for (size_t index = 0U; index < NVIDIA_DRIVER_COUNT; ++index) {
        const struct nvidia_driver_record *record = &nvidia_drivers[index];
        const struct pci_function *function = find_function(record);
        enum nvidia_status status;

        if (function == NULL) {
            continue;
        }
        ++installed_result.present;
        status = bind_one(record, function, &installed_result.probes[index]);
        if (status != NVIDIA_STATUS_OK) {
            if (installed_result.failed_status == NVIDIA_STATUS_OK) {
                installed_result.failed_status = status;
                installed_result.failed_driver = (uint32_t)index;
            }
            continue;
        }
        ++installed_result.bound;
    }
    capture_census(&after);

    installed_result.register_reads = register_reads;
    installed_result.register_writes = register_writes;
    installed_result.any_function_present = installed_result.present != 0U;
    installed_result.every_present_function_bound =
        installed_result.bound == installed_result.present;
    /*
     * The binding is over before the teardown is judged: nvidia_resources_
     * released() reports on a module that is not mid-bind, so asking it while
     * the flag is still set would answer about the wrong moment.
     */
    nvidia_active = false;
    installed_result.teardown_complete = nvidia_resources_released();
    installed_result.resource_census_equal = census_equal(&before, &after);
    *result = installed_result;

    if (!installed_result.teardown_complete) {
        return NVIDIA_STATUS_RELEASE_FAILURE;
    }
    if (!installed_result.resource_census_equal) {
        return NVIDIA_STATUS_RESOURCE_CENSUS;
    }
    if (installed_result.failed_status != NVIDIA_STATUS_OK) {
        return installed_result.failed_status;
    }
    /*
     * No NVIDIA function present is a healthy answer, not a failure: it is the
     * only answer this kernel has ever actually observed.
     */
    return NVIDIA_STATUS_OK;
}

struct nvidia_result nvidia_get_result(void)
{
    return installed_result;
}

bool nvidia_resources_released(void)
{
    const struct pci_resource_state state = pci_resource_get_state();

    return !nvidia_active && state.active_claims == 0U &&
        state.active_mappings == 0U && state.mapped_pages == 0U &&
        state.bus_masters == 0U;
}

const char *nvidia_status_string(enum nvidia_status status)
{
    static const char *const messages[NVIDIA_STATUS_COUNT] = {
        "ok",
        "null NVIDIA argument",
        "NVIDIA drivers are already binding",
        "NVIDIA prerequisite missing",
        "NVIDIA device claim failed",
        "NVIDIA register window mapping failed",
        "NVIDIA register window is too small",
        "NVIDIA register aperture is big-endian",
        "NVIDIA device identity was refused",
        "NVIDIA timer did not advance",
        "NVIDIA video BIOS window is empty",
        "NVIDIA video BIOS image is malformed",
        "NVIDIA ROM shadow bit was not restored",
        "NVIDIA function reported an unsupported version",
        "NVIDIA device release failed",
        "NVIDIA resource census changed",
        "NVIDIA controlled self-test failed"
    };

    _Static_assert(sizeof(messages) / sizeof(messages[0]) ==
        NVIDIA_STATUS_COUNT, "NVIDIA status messages drifted");
    if (status < NVIDIA_STATUS_OK || status >= NVIDIA_STATUS_COUNT) {
        return "unknown NVIDIA status";
    }
    return messages[status];
}
