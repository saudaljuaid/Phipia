/*
 * A model of the NVIDIA graphics register interface, for driver testing.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * THIS IS NOT AN NVIDIA GPU AND IT DOES NOT EMULATE ONE. It presents the
 * handful of registers a bind-and-identify driver reads -- the master control
 * pair, the configuration-space mirror, the timer, the ROM shadow bit and the
 * PROM window -- so that such a driver can be executed end to end without the
 * silicon. There is no graphics engine, no channel, no display, no interrupt
 * and no memory management here, and nothing in this file should be taken as a
 * description of how real hardware behaves beyond those few registers.
 *
 * The values it answers with are deliberately NOT invented here: the boot
 * register comes from a command-line property and the ROM image is read from a
 * file the caller supplies, so a driver tested against this model is checked
 * against values pinned outside both the driver and this model. The timer is
 * driven by QEMU's own clock, so a driver's "the count advances" check is a
 * real one.
 *
 * The register offsets are the ones the envytools project documents and the
 * Nouveau driver uses. Those offsets are the one thing this model and the
 * driver under test share, and a mistake in reading that documentation would
 * appear identically in both. That limit is inherent to a model and is why it
 * is not a substitute for hardware.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"

#define TYPE_SAPOTE_NVIDIA_MODEL "sapote-nvidia-model"
OBJECT_DECLARE_SIMPLE_TYPE(SapoteNvidiaState, SAPOTE_NVIDIA_MODEL)

/* envytools hw/pmc.txt */
#define NV_PMC_BOOT_0            0x000000
#define NV_PMC_BOOT_1            0x000004
/* envytools hw/ptimer.txt */
#define NV_PTIMER_TIME_0         0x009400
#define NV_PTIMER_TIME_1         0x009410
/* Nouveau nvkm/subdev/pci: the configuration mirror on NV50 and later. */
#define NV_PBUS_PCI_MIRROR       0x088000
#define NV_PBUS_PCI_MIRROR_BYTES 0x1000
#define NV_PBUS_PCI_NV_20        0x088050
#define NV_ROM_SHADOW_BIT        0x00000001
/* Nouveau nvkm/subdev/bios/shadowrom.c */
#define NV_PROM_BASE             0x300000
#define NV_PROM_BYTES            0x10000

#define SAPOTE_NVIDIA_BAR_BYTES  (16 * MiB)
#define NVIDIA_VENDOR_ID         0x10DE

struct SapoteNvidiaState {
    PCIDevice parent_obj;
    MemoryRegion registers;

    /* Pinned by the caller so the driver is checked against outside values. */
    uint32_t boot0;
    uint32_t boot1;
    char *vbios_path;

    uint32_t shadow;
    uint8_t rom[NV_PROM_BYTES];
    bool rom_present;
};

static uint64_t sapote_nvidia_read(void *opaque, hwaddr addr, unsigned size)
{
    SapoteNvidiaState *state = opaque;

    if (addr >= NV_PROM_BASE && addr < NV_PROM_BASE + NV_PROM_BYTES) {
        uint64_t value = 0;
        hwaddr offset = addr - NV_PROM_BASE;

        /*
         * While the shadow bit is set the window answers with the shadow copy
         * rather than the ROM, which is exactly why a driver has to clear it.
         */
        for (unsigned index = 0; index < size; ++index) {
            uint8_t byte = 0xFF;

            if (!(state->shadow & NV_ROM_SHADOW_BIT) && state->rom_present &&
                offset + index < NV_PROM_BYTES) {
                byte = state->rom[offset + index];
            }
            value |= (uint64_t)byte << (index * 8);
        }
        return value;
    }

    if (addr >= NV_PBUS_PCI_MIRROR &&
        addr < NV_PBUS_PCI_MIRROR + NV_PBUS_PCI_MIRROR_BYTES) {
        uint32_t offset = addr - NV_PBUS_PCI_MIRROR;

        if (offset == (NV_PBUS_PCI_NV_20 - NV_PBUS_PCI_MIRROR)) {
            return state->shadow;
        }
        /*
         * The mirror is the device's own configuration space, read through
         * the same accessor the configuration cycles use. It is a mirror, not
         * a copy: a driver comparing the two paths is comparing one source.
         */
        return pci_default_read_config(PCI_DEVICE(state), offset, size);
    }

    switch (addr) {
    case NV_PMC_BOOT_0:
        return state->boot0;
    case NV_PMC_BOOT_1:
        return state->boot1;
    case NV_PTIMER_TIME_0:
        return (uint32_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    case NV_PTIMER_TIME_1:
        return (uint32_t)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >> 32);
    default:
        return 0;
    }
}

static void sapote_nvidia_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    SapoteNvidiaState *state = opaque;

    /* The shadow bit is the only writable register this model has. */
    if (addr == NV_PBUS_PCI_NV_20) {
        state->shadow = (uint32_t)value;
    }
}

static const MemoryRegionOps sapote_nvidia_ops = {
    .read = sapote_nvidia_read,
    .write = sapote_nvidia_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void sapote_nvidia_realize(PCIDevice *dev, Error **errp)
{
    SapoteNvidiaState *state = SAPOTE_NVIDIA_MODEL(dev);

    if (state->vbios_path) {
        GError *error = NULL;
        gchar *contents = NULL;
        gsize length = 0;

        if (!g_file_get_contents(state->vbios_path, &contents, &length,
                                 &error)) {
            error_setg(errp, "could not read the ROM image: %s",
                       error ? error->message : "unknown error");
            g_clear_error(&error);
            return;
        }
        if (length > NV_PROM_BYTES) {
            length = NV_PROM_BYTES;
        }
        memset(state->rom, 0xFF, sizeof(state->rom));
        memcpy(state->rom, contents, length);
        state->rom_present = true;
        g_free(contents);
    }

    /* Powers up with the shadow enabled, which is what makes it worth a bit. */
    state->shadow = NV_ROM_SHADOW_BIT;

    memory_region_init_io(&state->registers, OBJECT(state),
                          &sapote_nvidia_ops, state,
                          "sapote-nvidia-model/registers",
                          SAPOTE_NVIDIA_BAR_BYTES);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_32, &state->registers);
}

static Property sapote_nvidia_properties[] = {
    DEFINE_PROP_UINT32("boot0", SapoteNvidiaState, boot0, 0x134000A1),
    DEFINE_PROP_UINT32("boot1", SapoteNvidiaState, boot1, 0x00000000),
    DEFINE_PROP_STRING("vbios", SapoteNvidiaState, vbios_path),
    DEFINE_PROP_END_OF_LIST(),
};

static void sapote_nvidia_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pci = PCI_DEVICE_CLASS(klass);

    pci->realize = sapote_nvidia_realize;
    pci->vendor_id = NVIDIA_VENDOR_ID;
    pci->device_id = 0x1B80;
    pci->revision = 0xA1;
    pci->class_id = PCI_CLASS_DISPLAY_VGA;
    dc->desc = "Model of the NVIDIA register interface (not a GPU)";
    device_class_set_props(dc, sapote_nvidia_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo sapote_nvidia_info = {
    .name = TYPE_SAPOTE_NVIDIA_MODEL,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(SapoteNvidiaState),
    .class_init = sapote_nvidia_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void sapote_nvidia_register_types(void)
{
    type_register_static(&sapote_nvidia_info);
}

type_init(sapote_nvidia_register_types)
