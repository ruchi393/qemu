/*
 * ITS emulation for a GICv3-based system
 *
 * Copyright Linaro.org 2021
 *
 * Authors:
 *  Shashi Mallela <shashi.mallela@linaro.org>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or (at your
 * option) any later version.  See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/qdev-properties.h"
#include "hw/intc/arm_gicv3_its_common.h"
#include "gicv3_internal.h"
#include "qom/object.h"

typedef struct GICv3ITSClass GICv3ITSClass;
/* This is reusing the GICv3ITSState typedef from ARM_GICV3_ITS_COMMON */
DECLARE_OBJ_CHECKERS(GICv3ITSState, GICv3ITSClass,
                     ARM_GICV3_ITS, TYPE_ARM_GICV3_ITS)

struct GICv3ITSClass {
    GICv3ITSCommonClass parent_class;
    void (*parent_reset)(DeviceState *dev);
};

static MemTxResult its_trans_writew(GICv3ITSState *s, hwaddr offset,
                               uint64_t value, MemTxAttrs attrs)
{
    MemTxResult result = MEMTX_OK;

    return result;
}

static MemTxResult its_trans_writel(GICv3ITSState *s, hwaddr offset,
                               uint64_t value, MemTxAttrs attrs)
{
    MemTxResult result = MEMTX_OK;

    return result;
}

static MemTxResult gicv3_its_trans_write(void *opaque, hwaddr offset,
                               uint64_t data, unsigned size, MemTxAttrs attrs)
{
    GICv3ITSState *s = (GICv3ITSState *)opaque;
    MemTxResult result;

    switch (size) {
    case 2:
        result = its_trans_writew(s, offset, data, attrs);
        break;
    case 4:
        result = its_trans_writel(s, offset, data, attrs);
        break;
    default:
        result = MEMTX_ERROR;
        break;
    }

    if (result == MEMTX_ERROR) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid guest write at offset " TARGET_FMT_plx
                      "size %u\n", __func__, offset, size);
        /*
         * The spec requires that reserved registers are RAZ/WI;
         * so use MEMTX_ERROR returns from leaf functions as a way to
         * trigger the guest-error logging but don't return it to
         * the caller, or we'll cause a spurious guest data abort.
         */
        result = MEMTX_OK;
    }
    return result;
}

static MemTxResult gicv3_its_trans_read(void *opaque, hwaddr offset,
                              uint64_t *data, unsigned size, MemTxAttrs attrs)
{
    qemu_log_mask(LOG_GUEST_ERROR,
        "%s: Invalid read from transaction register area at offset "
        TARGET_FMT_plx "\n", __func__, offset);
    return MEMTX_ERROR;
}

static MemTxResult its_writeb(GICv3ITSState *s, hwaddr offset,
                               uint64_t value, MemTxAttrs attrs)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                "%s: unsupported byte write to register at offset "
                TARGET_FMT_plx "\n", __func__, offset);
    return MEMTX_ERROR;
}

static MemTxResult its_readb(GICv3ITSState *s, hwaddr offset,
                               uint64_t *data, MemTxAttrs attrs)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                "%s: unsupported byte read from register at offset "
                TARGET_FMT_plx "\n", __func__, offset);
    return MEMTX_ERROR;
}

static MemTxResult its_writew(GICv3ITSState *s, hwaddr offset,
                               uint64_t value, MemTxAttrs attrs)
{
    qemu_log_mask(LOG_GUEST_ERROR,
        "%s: unsupported word write to register at offset "
        TARGET_FMT_plx "\n", __func__, offset);
    return MEMTX_ERROR;
}

static MemTxResult its_readw(GICv3ITSState *s, hwaddr offset,
                               uint64_t *data, MemTxAttrs attrs)
{
    qemu_log_mask(LOG_GUEST_ERROR,
        "%s: unsupported word read from register at offset "
        TARGET_FMT_plx "\n", __func__, offset);
    return MEMTX_ERROR;
}

static MemTxResult its_writel(GICv3ITSState *s, hwaddr offset,
                               uint64_t value, MemTxAttrs attrs)
{
    MemTxResult result = MEMTX_OK;

    return result;
}

static MemTxResult its_readl(GICv3ITSState *s, hwaddr offset,
                               uint64_t *data, MemTxAttrs attrs)
{
    MemTxResult result = MEMTX_OK;

    return result;
}

static MemTxResult its_writell(GICv3ITSState *s, hwaddr offset,
                               uint64_t value, MemTxAttrs attrs)
{
    MemTxResult result = MEMTX_OK;

    return result;
}

static MemTxResult its_readll(GICv3ITSState *s, hwaddr offset,
                               uint64_t *data, MemTxAttrs attrs)
{
    MemTxResult result = MEMTX_OK;

    return result;
}

static MemTxResult gicv3_its_read(void *opaque, hwaddr offset, uint64_t *data,
                              unsigned size, MemTxAttrs attrs)
{
    GICv3ITSState *s = (GICv3ITSState *)opaque;
    MemTxResult result;

    switch (size) {
    case 1:
        result = its_readb(s, offset, data, attrs);
        break;
    case 2:
        result = its_readw(s, offset, data, attrs);
        break;
    case 4:
        result = its_readl(s, offset, data, attrs);
        break;
    case 8:
        result = its_readll(s, offset, data, attrs);
        break;
    default:
        result = MEMTX_ERROR;
        break;
    }

    if (result == MEMTX_ERROR) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid guest read at offset " TARGET_FMT_plx
                      "size %u\n", __func__, offset, size);
        /*
         * The spec requires that reserved registers are RAZ/WI;
         * so use MEMTX_ERROR returns from leaf functions as a way to
         * trigger the guest-error logging but don't return it to
         * the caller, or we'll cause a spurious guest data abort.
         */
        result = MEMTX_OK;
        *data = 0;
    }
    return result;
}

static MemTxResult gicv3_its_write(void *opaque, hwaddr offset, uint64_t data,
                               unsigned size, MemTxAttrs attrs)
{
    GICv3ITSState *s = (GICv3ITSState *)opaque;
    MemTxResult result;

    switch (size) {
    case 1:
        result = its_writeb(s, offset, data, attrs);
        break;
    case 2:
        result = its_writew(s, offset, data, attrs);
        break;
    case 4:
        result = its_writel(s, offset, data, attrs);
        break;
    case 8:
        result = its_writell(s, offset, data, attrs);
        break;
    default:
        result = MEMTX_ERROR;
        break;
    }

    if (result == MEMTX_ERROR) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid guest write at offset " TARGET_FMT_plx
                      "size %u\n", __func__, offset, size);
        /*
         * The spec requires that reserved registers are RAZ/WI;
         * so use MEMTX_ERROR returns from leaf functions as a way to
         * trigger the guest-error logging but don't return it to
         * the caller, or we'll cause a spurious guest data abort.
         */
        result = MEMTX_OK;
    }
    return result;
}

static const MemoryRegionOps gicv3_its_control_ops = {
    .read_with_attrs = gicv3_its_read,
    .write_with_attrs = gicv3_its_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const MemoryRegionOps gicv3_its_trans_ops = {
    .read_with_attrs = gicv3_its_trans_read,
    .write_with_attrs = gicv3_its_trans_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void gicv3_arm_its_realize(DeviceState *dev, Error **errp)
{
    GICv3ITSState *s = ARM_GICV3_ITS_COMMON(dev);

    gicv3_its_init_mmio(s, &gicv3_its_control_ops, &gicv3_its_trans_ops);
}

static void gicv3_its_reset(DeviceState *dev)
{
    GICv3ITSState *s = ARM_GICV3_ITS_COMMON(dev);
    GICv3ITSClass *c = ARM_GICV3_ITS_GET_CLASS(s);

    if (s->gicv3->cpu->gicr_typer & GICR_TYPER_PLPIS) {
        c->parent_reset(dev);

        /* set the ITS default features supported */
        s->typer = GITS_TYPER_PHYSICAL | (GITS_TYPER_ITT_ENTRY_SIZE <<
                   GITS_TYPER_ITT_ENTRY_OFFSET) | (GITS_TYPER_IDBITS <<
                   GITS_TYPER_IDBITS_OFFSET) | GITS_TYPER_DEVBITS |
                   GITS_TYPER_CIL | GITS_TYPER_CIDBITS;

        /*
         * We claim to be an ARM r0p0 with a zero ProductID.
         * This is the same as an r0p0 GIC-500.
         */
        s->iidr = gicv3_iidr();

        /* Quiescent bit reset to 1 */
        s->ctlr = (1U << 31);

        /*
         * setting GITS_BASER0.Type = 0b001 (Device)
         *         GITS_BASER1.Type = 0b100 (Collection Table)
         *         GITS_BASER<n>.Type,where n = 3 to 7 are 0b00 (Unimplemented)
         *         GITS_BASER<0,1>.Page_Size = 64KB
         * and default translation table entry size to 16 bytes
         */
        s->baser[0] = (GITS_ITT_TYPE_DEVICE << GITS_BASER_TYPE_OFFSET) |
                      (GITS_BASER_PAGESIZE_64K << GITS_BASER_PAGESIZE_OFFSET) |
                      (GITS_DTE_SIZE << GITS_BASER_ENTRYSIZE_OFFSET);
        s->baser[1] = (GITS_ITT_TYPE_COLLECTION << GITS_BASER_TYPE_OFFSET) |
                      (GITS_BASER_PAGESIZE_64K << GITS_BASER_PAGESIZE_OFFSET) |
                      (GITS_CTE_SIZE << GITS_BASER_ENTRYSIZE_OFFSET);
    }
}

static Property gicv3_its_props[] = {
    DEFINE_PROP_LINK("parent-gicv3", GICv3ITSState, gicv3, "arm-gicv3",
                     GICv3State *),
    DEFINE_PROP_END_OF_LIST(),
};

static void gicv3_its_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    GICv3ITSClass *ic = ARM_GICV3_ITS_CLASS(klass);

    dc->realize = gicv3_arm_its_realize;
    device_class_set_props(dc, gicv3_its_props);
    device_class_set_parent_reset(dc, gicv3_its_reset, &ic->parent_reset);
}

static const TypeInfo gicv3_its_info = {
    .name = TYPE_ARM_GICV3_ITS,
    .parent = TYPE_ARM_GICV3_ITS_COMMON,
    .instance_size = sizeof(GICv3ITSState),
    .class_init = gicv3_its_class_init,
    .class_size = sizeof(GICv3ITSClass),
};

static void gicv3_its_register_types(void)
{
    type_register_static(&gicv3_its_info);
}

type_init(gicv3_its_register_types)
