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

typedef struct {
    bool valid;
    bool indirect;
    uint16_t entry_sz;
    uint32_t max_entries;
    uint32_t max_devids;
    uint64_t base_addr;
} DevTableDesc;

typedef struct {
    bool valid;
    bool indirect;
    uint16_t entry_sz;
    uint32_t max_entries;
    uint32_t max_collids;
    uint64_t base_addr;
} CollTableDesc;

typedef struct {
    bool valid;
    uint32_t max_entries;
    uint64_t base_addr;
} CmdQDesc;

struct GICv3ITSClass {
    GICv3ITSCommonClass parent_class;
    void (*parent_reset)(DeviceState *dev);

    DevTableDesc  dt;
    CollTableDesc ct;
    CmdQDesc      cq;
};

typedef enum ItsCmdType {
    NONE = 0, /* internal indication for GITS_TRANSLATER write */
    CLEAR = 1,
    DISCARD = 2,
    INT = 3,
} ItsCmdType;

static bool get_cte(GICv3ITSState *s, uint16_t icid, uint64_t *cte)
{
    GICv3ITSClass *c = ARM_GICV3_ITS_GET_CLASS(s);
    AddressSpace *as = &s->gicv3->sysmem_as;
    uint8_t  page_sz_type;
    uint64_t l2t_addr;
    uint64_t value;
    bool valid_l2t;
    uint32_t l2t_id;
    uint32_t page_sz = 0;
    uint32_t max_l2_entries;
    bool status = false;

    if (c->ct.indirect) {
        /* 2 level table */
        page_sz_type = (s->baser[0] >>
                        GITS_BASER_PAGESIZE_OFFSET) &
                        GITS_BASER_PAGESIZE_MASK;

        if (page_sz_type == 0) {
            page_sz = GITS_ITT_PAGE_SIZE_0;
        } else if (page_sz_type == 1) {
            page_sz = GITS_ITT_PAGE_SIZE_1;
        } else if (page_sz_type == 2) {
            page_sz = GITS_ITT_PAGE_SIZE_2;
        }

        l2t_id = icid / (page_sz / L1TABLE_ENTRY_SIZE);

        value = address_space_ldq_le(as,
                                     c->ct.base_addr +
                                     (l2t_id * L1TABLE_ENTRY_SIZE),
                                     MEMTXATTRS_UNSPECIFIED, NULL);

        valid_l2t = (value >> VALID_SHIFT) & VALID_MASK;

        if (valid_l2t) {
            max_l2_entries = page_sz / c->ct.entry_sz;

            l2t_addr = (value >> page_sz_type) &
                        ((1ULL << (51 - page_sz_type)) - 1);

            address_space_read(as, l2t_addr +
                                 ((icid % max_l2_entries) * GITS_CTE_SIZE),
                                 MEMTXATTRS_UNSPECIFIED,
                                 cte, sizeof(*cte));
       }
    } else {
        /* Flat level table */
        address_space_read(as, c->ct.base_addr + (icid * GITS_CTE_SIZE),
                            MEMTXATTRS_UNSPECIFIED, cte,
                            sizeof(*cte));
    }

    if (*cte & VALID_MASK) {
        status = true;
    }

    return status;
}

static bool get_ite(GICv3ITSState *s, uint32_t eventid, uint64_t dte,
                      uint16_t *icid, uint32_t *pIntid)
{
    AddressSpace *as = &s->gicv3->sysmem_as;
    uint8_t buff[GITS_TYPER_ITT_ENTRY_SIZE];
    uint64_t itt_addr;
    bool status = false;

    itt_addr = (dte >> 6ULL) & ITTADDR_MASK;
    itt_addr <<= ITTADDR_OFFSET; /* 256 byte aligned */

    address_space_read(as, itt_addr + (eventid * sizeof(buff)),
                MEMTXATTRS_UNSPECIFIED, &buff,
                sizeof(buff));

    if (buff[0] & VALID_MASK) {
        if ((buff[0] >> 1U) & GITS_TYPER_PHYSICAL) {
            memcpy(pIntid, &buff[1], 3);
            memcpy(icid, &buff[7], sizeof(*icid));
            status = true;
        }
    }

    return status;
}

static uint64_t get_dte(GICv3ITSState *s, uint32_t devid)
{
    GICv3ITSClass *c = ARM_GICV3_ITS_GET_CLASS(s);
    AddressSpace *as = &s->gicv3->sysmem_as;
    uint8_t  page_sz_type;
    uint64_t l2t_addr;
    uint64_t value;
    bool valid_l2t;
    uint32_t l2t_id;
    uint32_t page_sz = 0;
    uint32_t max_l2_entries;

    if (c->ct.indirect) {
        /* 2 level table */
        page_sz_type = (s->baser[0] >>
                        GITS_BASER_PAGESIZE_OFFSET) &
                        GITS_BASER_PAGESIZE_MASK;

        if (page_sz_type == 0) {
            page_sz = GITS_ITT_PAGE_SIZE_0;
        } else if (page_sz_type == 1) {
            page_sz = GITS_ITT_PAGE_SIZE_1;
        } else if (page_sz_type == 2) {
            page_sz = GITS_ITT_PAGE_SIZE_2;
        }

        l2t_id = devid / (page_sz / L1TABLE_ENTRY_SIZE);

        value = address_space_ldq_le(as,
                                     c->dt.base_addr +
                                     (l2t_id * L1TABLE_ENTRY_SIZE),
                                     MEMTXATTRS_UNSPECIFIED, NULL);

        valid_l2t = (value >> VALID_SHIFT) & VALID_MASK;

        if (valid_l2t) {
            max_l2_entries = page_sz / c->dt.entry_sz;

            l2t_addr = (value >> page_sz_type) &
                        ((1ULL << (51 - page_sz_type)) - 1);

            value = 0;
            address_space_read(as, l2t_addr +
                                 ((devid % max_l2_entries) * GITS_DTE_SIZE),
                                 MEMTXATTRS_UNSPECIFIED,
                                 &value, sizeof(value));
        }
    } else {
        /* Flat level table */
        value = 0;
        address_space_read(as, c->dt.base_addr + (devid * GITS_DTE_SIZE),
                            MEMTXATTRS_UNSPECIFIED, &value,
                            sizeof(value));
    }

    return value;
}

static MemTxResult process_sync(GICv3ITSState *s, uint32_t offset)
{
    GICv3ITSClass *c = ARM_GICV3_ITS_GET_CLASS(s);
    AddressSpace *as = &s->gicv3->sysmem_as;
    uint64_t rdbase;
    uint64_t value;
    bool pta = false;
    MemTxResult res = MEMTX_OK;

    offset += NUM_BYTES_IN_DW;
    offset += NUM_BYTES_IN_DW;

    value = address_space_ldq_le(as, c->cq.base_addr + offset,
                                     MEMTXATTRS_UNSPECIFIED, &res);

    if ((s->typer >> GITS_TYPER_PTA_OFFSET) & GITS_TYPER_PTA_MASK) {
        /*
         * only bits[47:16] are considered instead of bits [51:16]
         * since with a physical address the target address must be
         * 64KB aligned
         */
        rdbase = (value >> RDBASE_OFFSET) & RDBASE_MASK;
        pta = true;
    } else {
        rdbase = (value >> RDBASE_OFFSET) & RDBASE_PROCNUM_MASK;
    }

    if (!pta && (rdbase < (s->gicv3->num_cpu))) {
        /*
         * Current implementation makes a blocking synchronous call
         * for every command issued earlier,hence the internal state
         * is already consistent by the time SYNC command is executed.
         */
    }

    offset += NUM_BYTES_IN_DW;
    return res;
}

static MemTxResult process_int(GICv3ITSState *s, uint64_t value,
                                uint32_t offset, ItsCmdType cmd)
{
    GICv3ITSClass *c = ARM_GICV3_ITS_GET_CLASS(s);
    AddressSpace *as = &s->gicv3->sysmem_as;
    uint32_t devid, eventid;
    MemTxResult res = MEMTX_OK;
    bool dte_valid;
    uint64_t dte = 0;
    uint32_t max_eventid;
    uint16_t icid = 0;
    uint32_t pIntid = 0;
    bool ite_valid = false;
    uint64_t cte = 0;
    bool cte_valid = false;
    uint64_t rdbase;
    uint8_t buff[GITS_TYPER_ITT_ENTRY_SIZE];
    uint64_t itt_addr;

    if (cmd == NONE) {
        devid = offset;
    } else {
        devid = (value >> DEVID_OFFSET) & DEVID_MASK;

        offset += NUM_BYTES_IN_DW;
        value = address_space_ldq_le(as, c->cq.base_addr + offset,
                                 MEMTXATTRS_UNSPECIFIED, &res);
    }

    eventid = (value & EVENTID_MASK);

    dte = get_dte(s, devid);
    dte_valid = dte & VALID_MASK;

    if (dte_valid) {
        max_eventid = (1UL << (((dte >> 1U) & SIZE_MASK) + 1));

        ite_valid = get_ite(s, eventid, dte, &icid, &pIntid);

        if (ite_valid) {
            cte_valid = get_cte(s, icid, &cte);
        }
    }

    if ((devid > c->dt.max_devids) || !dte_valid || !ite_valid ||
            !cte_valid || (eventid > max_eventid)) {
        if ((s->typer >> GITS_TYPER_SEIS_OFFSET) &
                         GITS_TYPER_SEIS_MASK) {
            /*
             * Generate System Error here if supported
             * for each of the individual error cases
             */
        }
        qemu_log_mask(LOG_GUEST_ERROR,
            "%s: invalid interrupt translation table attributes "
            "devid %d or eventid %d\n",
            __func__, devid, eventid);
        /*
         * in this implementation,in case of error
         * we ignore this command and move onto the next
         * command in the queue
         */
    } else {
        if ((s->typer >> GITS_TYPER_PTA_OFFSET) & GITS_TYPER_PTA_MASK) {
            /*
             * only bits[47:16] are considered instead of bits [51:16]
             * since with a physical address the target address must be
             * 64KB aligned
             */
            rdbase = (cte >> 1U) & RDBASE_MASK;
            /*
             * Current implementation only supports rdbase == procnum
             * Hence rdbase physical address is ignored
             */
        } else {
            rdbase = (cte >> 1U) & RDBASE_PROCNUM_MASK;
            if ((cmd == CLEAR) || (cmd == DISCARD)) {
                gicv3_redist_process_lpi(&s->gicv3->cpu[rdbase], pIntid, 0);
            } else {
                gicv3_redist_process_lpi(&s->gicv3->cpu[rdbase], pIntid, 1);
            }

            if (cmd == DISCARD) {
                /* remove mapping from interrupt translation table */
                memset(buff, 0, sizeof(buff));

                itt_addr = (dte >> 6ULL) & ITTADDR_MASK;
                itt_addr <<= ITTADDR_OFFSET; /* 256 byte aligned */

                address_space_write(as, itt_addr + (eventid * sizeof(buff)),
                                    MEMTXATTRS_UNSPECIFIED, &buff,
                                    sizeof(buff));
            }
        }
    }

    if (cmd != NONE) {
        offset += NUM_BYTES_IN_DW;
        offset += NUM_BYTES_IN_DW;
    }

    return res;
}

static MemTxResult process_mapti(GICv3ITSState *s, uint64_t value,
                                    uint32_t offset, bool ignore_pInt)
{
    GICv3ITSClass *c = ARM_GICV3_ITS_GET_CLASS(s);
    AddressSpace *as = &s->gicv3->sysmem_as;
    uint32_t devid, eventid;
    uint32_t pIntid = 0;
    uint32_t max_eventid, max_Intid;
    bool dte_valid;
    MemTxResult res = MEMTX_OK;
    uint16_t icid = 0;
    uint64_t dte = 0;
    uint64_t itt_addr;
    uint8_t buff[GITS_TYPER_ITT_ENTRY_SIZE];
    uint32_t int_spurious = INTID_SPURIOUS;

    devid = (value >> DEVID_OFFSET) & DEVID_MASK;
    offset += NUM_BYTES_IN_DW;
    value = address_space_ldq_le(as, c->cq.base_addr + offset,
                                 MEMTXATTRS_UNSPECIFIED, &res);

    eventid = (value & EVENTID_MASK);

    if (!ignore_pInt) {
        pIntid = (value >> pINTID_OFFSET) & pINTID_MASK;
    }

    offset += NUM_BYTES_IN_DW;
    value = address_space_ldq_le(as, c->cq.base_addr + offset,
                                 MEMTXATTRS_UNSPECIFIED, &res);

    icid = value & ICID_MASK;

    dte = get_dte(s, devid);
    dte_valid = dte & VALID_MASK;

    max_eventid = (1UL << (((dte >> 1U) & SIZE_MASK) + 1));

    if (!ignore_pInt) {
        max_Intid = (1UL << (((s->typer >> GITS_TYPER_IDBITS_OFFSET) &
                      GITS_TYPER_IDBITS_MASK) + 1));
    }

    if ((devid > c->dt.max_devids) || (icid > c->ct.max_collids) ||
            !dte_valid || (eventid > max_eventid) ||
            (!ignore_pInt && ((pIntid < GICV3_LPI_INTID_START) ||
               (pIntid > max_Intid)))) {
        if ((s->typer >> GITS_TYPER_SEIS_OFFSET) &
                         GITS_TYPER_SEIS_MASK) {
            /*
             * Generate System Error here if supported
             * for each of the individual error cases
             */
        }
        qemu_log_mask(LOG_GUEST_ERROR,
            "%s: invalid interrupt translation table attributes "
            "devid %d or icid %d or eventid %d or pIntid %d\n",
            __func__, devid, icid, eventid, pIntid);
        /*
         * in this implementation,in case of error
         * we ignore this command and move onto the next
         * command in the queue
         */
    } else {
        /* add entry to interrupt translation table */
        memset(buff, 0, sizeof(buff));
        buff[0] = (dte_valid & VALID_MASK) | (GITS_TYPER_PHYSICAL << 1U);
        if (ignore_pInt) {
            memcpy(&buff[1], &eventid, 3);
        } else {
            memcpy(&buff[1], &pIntid, 3);
        }
        memcpy(&buff[4], &int_spurious, 3);
        memcpy(&buff[7], &icid, sizeof(icid));

        itt_addr = (dte >> 6ULL) & ITTADDR_MASK;
        itt_addr <<= ITTADDR_OFFSET; /* 256 byte aligned */

        address_space_write(as, itt_addr + (eventid * sizeof(buff)),
                    MEMTXATTRS_UNSPECIFIED, &buff,
                    sizeof(buff));
    }

    offset += NUM_BYTES_IN_DW;
    offset += NUM_BYTES_IN_DW;

    return res;
}

static void update_cte(GICv3ITSState *s, uint16_t icid, uint64_t cte)
{
    GICv3ITSClass *c = ARM_GICV3_ITS_GET_CLASS(s);
    AddressSpace *as = &s->gicv3->sysmem_as;
    uint64_t value;
    uint8_t  page_sz_type;
    uint64_t l2t_addr;
    bool valid_l2t;
    uint32_t l2t_id;
    uint32_t page_sz = 0;
    uint32_t max_l2_entries;

    if (c->ct.indirect) {
        /* 2 level table */
        page_sz_type = (s->baser[0] >>
                        GITS_BASER_PAGESIZE_OFFSET) &
                        GITS_BASER_PAGESIZE_MASK;

        if (page_sz_type == 0) {
            page_sz = GITS_ITT_PAGE_SIZE_0;
        } else if (page_sz_type == 1) {
            page_sz = GITS_ITT_PAGE_SIZE_1;
        } else if (page_sz_type == 2) {
            page_sz = GITS_ITT_PAGE_SIZE_2;
        }

        l2t_id = icid / (page_sz / L1TABLE_ENTRY_SIZE);

        value = address_space_ldq_le(as,
                                     c->ct.base_addr +
                                     (l2t_id * L1TABLE_ENTRY_SIZE),
                                     MEMTXATTRS_UNSPECIFIED, NULL);

        valid_l2t = (value >> VALID_SHIFT) & VALID_MASK;

        if (valid_l2t) {
            max_l2_entries = page_sz / c->ct.entry_sz;

            l2t_addr = (value >> page_sz_type) &
                        ((1ULL << (51 - page_sz_type)) - 1);

            address_space_write(as, l2t_addr +
                                 ((icid % max_l2_entries) * GITS_CTE_SIZE),
                                 MEMTXATTRS_UNSPECIFIED,
                                 &cte, sizeof(cte));
        }
    } else {
        /* Flat level table */
        address_space_write(as, c->ct.base_addr + (icid * GITS_CTE_SIZE),
                            MEMTXATTRS_UNSPECIFIED, &cte,
                            sizeof(cte));
    }
}

static MemTxResult process_mapc(GICv3ITSState *s, uint32_t offset)
{
    GICv3ITSClass *c = ARM_GICV3_ITS_GET_CLASS(s);
    AddressSpace *as = &s->gicv3->sysmem_as;
    uint16_t icid;
    uint64_t rdbase;
    bool valid;
    bool pta = false;
    MemTxResult res = MEMTX_OK;
    uint64_t cte_entry;
    uint64_t value;

    offset += NUM_BYTES_IN_DW;
    offset += NUM_BYTES_IN_DW;

    value = address_space_ldq_le(as, c->cq.base_addr + offset,
                                 MEMTXATTRS_UNSPECIFIED, &res);

    icid = value & ICID_MASK;

    if ((s->typer >> GITS_TYPER_PTA_OFFSET) & GITS_TYPER_PTA_MASK) {
        /*
         * only bits[47:16] are considered instead of bits [51:16]
         * since with a physical address the target address must be
         * 64KB aligned
         */
        rdbase = (value >> RDBASE_OFFSET) & RDBASE_MASK;
        pta = true;
    } else {
        rdbase = (value >> RDBASE_OFFSET) & RDBASE_PROCNUM_MASK;
    }

    valid = (value >> VALID_SHIFT) & VALID_MASK;

    if (valid) {
        if ((icid > c->ct.max_collids) || (!pta &&
                (rdbase > s->gicv3->num_cpu))) {
            if ((s->typer >> GITS_TYPER_SEIS_OFFSET) &
                             GITS_TYPER_SEIS_MASK) {
                /* Generate System Error here if supported */
            }
            qemu_log_mask(LOG_GUEST_ERROR,
                "%s: invalid collection table attributes "
                "icid %d rdbase %lu\n", __func__, icid, rdbase);
            /*
             * in this implementation,in case of error
             * we ignore this command and move onto the next
             * command in the queue
             */
        } else {
            if (c->ct.valid) {
                /* add mapping entry to collection table */
                cte_entry = (valid & VALID_MASK) |
                            (pta ? ((rdbase & RDBASE_MASK) << 1ULL) :
                            ((rdbase & RDBASE_PROCNUM_MASK) << 1ULL));

                update_cte(s, icid, cte_entry);
            }
        }
    } else {
        if (c->ct.valid) {
            /* remove mapping entry from collection table */
            cte_entry = 0;

            update_cte(s, icid, cte_entry);
        }
    }

    offset += NUM_BYTES_IN_DW;
    offset += NUM_BYTES_IN_DW;

    return res;
}

static void update_dte(GICv3ITSState *s, uint32_t devid, uint64_t dte)
{
    GICv3ITSClass *c = ARM_GICV3_ITS_GET_CLASS(s);
    AddressSpace *as = &s->gicv3->sysmem_as;
    uint64_t value;
    uint8_t  page_sz_type;
    uint64_t l2t_addr;
    bool valid_l2t;
    uint32_t l2t_id;
    uint32_t page_sz = 0;
    uint32_t max_l2_entries;

    if (c->dt.indirect) {
        /* 2 level table */
        page_sz_type = (s->baser[0] >>
                        GITS_BASER_PAGESIZE_OFFSET) &
                        GITS_BASER_PAGESIZE_MASK;

        if (page_sz_type == 0) {
            page_sz = GITS_ITT_PAGE_SIZE_0;
        } else if (page_sz_type == 1) {
            page_sz = GITS_ITT_PAGE_SIZE_1;
        } else if (page_sz_type == 2) {
            page_sz = GITS_ITT_PAGE_SIZE_2;
        }

        l2t_id = devid / (page_sz / L1TABLE_ENTRY_SIZE);

        value = address_space_ldq_le(as,
                                     c->dt.base_addr +
                                     (l2t_id * L1TABLE_ENTRY_SIZE),
                                     MEMTXATTRS_UNSPECIFIED, NULL);

        valid_l2t = (value >> VALID_SHIFT) & VALID_MASK;

        if (valid_l2t) {
            max_l2_entries = page_sz / c->dt.entry_sz;

            l2t_addr = (value >> page_sz_type) &
                        ((1ULL << (51 - page_sz_type)) - 1);

            address_space_write(as, l2t_addr +
                                 ((devid % max_l2_entries) * GITS_DTE_SIZE),
                                 MEMTXATTRS_UNSPECIFIED, &dte, sizeof(dte));
        }
    } else {
        /* Flat level table */
        address_space_write(as, c->dt.base_addr + (devid * GITS_DTE_SIZE),
                            MEMTXATTRS_UNSPECIFIED, &dte, sizeof(dte));
    }
}

static MemTxResult process_mapd(GICv3ITSState *s, uint64_t value,
                                  uint32_t offset)
{
    GICv3ITSClass *c = ARM_GICV3_ITS_GET_CLASS(s);
    AddressSpace *as = &s->gicv3->sysmem_as;
    uint32_t devid;
    uint8_t size;
    uint64_t itt_addr;
    bool valid;
    MemTxResult res = MEMTX_OK;
    uint64_t dte_entry = 0;

    devid = (value >> DEVID_OFFSET) & DEVID_MASK;

    offset += NUM_BYTES_IN_DW;
    value = address_space_ldq_le(as, c->cq.base_addr + offset,
                                 MEMTXATTRS_UNSPECIFIED, &res);
    size = (value & SIZE_MASK);

    offset += NUM_BYTES_IN_DW;
    value = address_space_ldq_le(as, c->cq.base_addr + offset,
                                 MEMTXATTRS_UNSPECIFIED, &res);
    itt_addr = (value >> ITTADDR_OFFSET) & ITTADDR_MASK;

    valid = (value >> VALID_SHIFT) & VALID_MASK;

    if (valid) {
        if ((devid > c->dt.max_devids) ||
            (size > ((s->typer >> GITS_TYPER_IDBITS_OFFSET) &
                          GITS_TYPER_IDBITS_MASK))) {
            if ((s->typer >> GITS_TYPER_SEIS_OFFSET) &
                             GITS_TYPER_SEIS_MASK) {
                /* Generate System Error here if supported */
            }
            qemu_log_mask(LOG_GUEST_ERROR,
                "%s: invalid device table attributes "
                "devid %d or size %d\n", __func__, devid, size);
            /*
             * in this implementation,in case of error
             * we ignore this command and move onto the next
             * command in the queue
             */
        } else {
            if (c->dt.valid) {
                /* add mapping entry to device table */
                dte_entry = (valid & VALID_MASK) |
                            ((size & SIZE_MASK) << 1U) |
                            ((itt_addr & ITTADDR_MASK) << 6ULL);

                update_dte(s, devid, dte_entry);
            }
        }
    } else {
        if (c->dt.valid) {
            /* remove mapping entry from device table */
            dte_entry = 0;
            update_dte(s, devid, dte_entry);
        }
    }

    offset += NUM_BYTES_IN_DW;
    offset += NUM_BYTES_IN_DW;

    return res;
}

/*
 * Current implementation blocks until all
 * commands are processed
 */
static MemTxResult process_cmdq(GICv3ITSState *s)
{
    GICv3ITSClass *c = ARM_GICV3_ITS_GET_CLASS(s);
    uint32_t wr_offset = 0;
    uint32_t rd_offset = 0;
    uint32_t cq_offset = 0;
    uint64_t data;
    AddressSpace *as = &s->gicv3->sysmem_as;
    MemTxResult res = MEMTX_OK;
    uint8_t cmd;

    wr_offset = (s->cwriter >> GITS_CWRITER_OFFSET) &
                             GITS_CWRITER_OFFSET_MASK;

    if (wr_offset > c->cq.max_entries) {
        qemu_log_mask(LOG_GUEST_ERROR,
                        "%s: invalid write offset "
                        "%d\n", __func__, wr_offset);
        res = MEMTX_ERROR;
        return res;
    }

    rd_offset = (s->creadr >> GITS_CREADR_OFFSET) &
                             GITS_CREADR_OFFSET_MASK;

    while (wr_offset != rd_offset) {
        cq_offset = (rd_offset * GITS_CMDQ_ENTRY_SIZE);
        data = address_space_ldq_le(as, c->cq.base_addr + cq_offset,
                                      MEMTXATTRS_UNSPECIFIED, &res);
        cmd = (data & CMD_MASK);

        switch (cmd) {
        case GITS_CMD_INT:
            res = process_int(s, data, cq_offset, INT);
            break;
        case GITS_CMD_CLEAR:
            res = process_int(s, data, cq_offset, CLEAR);
            break;
        case GITS_CMD_SYNC:
            res = process_sync(s, cq_offset);
            break;
        case GITS_CMD_MAPD:
            res = process_mapd(s, data, cq_offset);
            break;
        case GITS_CMD_MAPC:
            res = process_mapc(s, cq_offset);
            break;
        case GITS_CMD_MAPTI:
            res = process_mapti(s, data, cq_offset, false);
            break;
        case GITS_CMD_MAPI:
            res = process_mapti(s, data, cq_offset, true);
            break;
        case GITS_CMD_DISCARD:
            res = process_int(s, data, cq_offset, DISCARD);
            break;
        default:
            break;
        }
        if (res == MEMTX_OK) {
            rd_offset++;
            rd_offset %= c->cq.max_entries;
            s->creadr = (rd_offset << GITS_CREADR_OFFSET);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                "%s: %x cmd processing failed!!\n", __func__, cmd);
            break;
        }
    }
    return res;
}

static bool extract_table_params(GICv3ITSState *s, int index)
{
    GICv3ITSClass *c = ARM_GICV3_ITS_GET_CLASS(s);
    uint16_t num_pages = 0;
    uint8_t  page_sz_type;
    uint8_t type;
    uint32_t page_sz = 0;
    uint64_t value = s->baser[index];

    num_pages = (value & GITS_BASER_SIZE);
    page_sz_type = (value >> GITS_BASER_PAGESIZE_OFFSET) &
                        GITS_BASER_PAGESIZE_MASK;

    if (page_sz_type == 0) {
        page_sz = GITS_ITT_PAGE_SIZE_0;
    } else if (page_sz_type == 0) {
        page_sz = GITS_ITT_PAGE_SIZE_1;
    } else if (page_sz_type == 2) {
        page_sz = GITS_ITT_PAGE_SIZE_2;
    } else {
        return false;
    }

    type = (value >> GITS_BASER_TYPE_OFFSET) &
                        GITS_BASER_TYPE_MASK;

    if (type == GITS_ITT_TYPE_DEVICE) {
        c->dt.valid = (value >> GITS_BASER_VALID) & GITS_BASER_VALID_MASK;

        if (c->dt.valid) {
            c->dt.indirect = (value >> GITS_BASER_INDIRECT_OFFSET) &
                                       GITS_BASER_INDIRECT_MASK;
            c->dt.entry_sz = (value >> GITS_BASER_ENTRYSIZE_OFFSET) &
                                   GITS_BASER_ENTRYSIZE_MASK;

            if (!c->dt.indirect) {
                c->dt.max_entries = ((num_pages + 1) * page_sz) /
                                                       c->dt.entry_sz;
            } else {
                c->dt.max_entries = ((((num_pages + 1) * page_sz) /
                                        L1TABLE_ENTRY_SIZE) *
                                    (page_sz / c->dt.entry_sz));
            }

            c->dt.max_devids = (1UL << (((value >> GITS_TYPER_DEVBITS_OFFSET) &
                                           GITS_TYPER_DEVBITS_MASK) + 1));

            if ((page_sz == GITS_ITT_PAGE_SIZE_0) ||
                    (page_sz == GITS_ITT_PAGE_SIZE_1)) {
                c->dt.base_addr = (value >> GITS_BASER_PHYADDR_OFFSET) &
                                        GITS_BASER_PHYADDR_MASK;
                c->dt.base_addr <<= GITS_BASER_PHYADDR_OFFSET;
            } else if (page_sz == GITS_ITT_PAGE_SIZE_2) {
                c->dt.base_addr = ((value >> GITS_BASER_PHYADDR_OFFSETL_64K) &
                                   GITS_BASER_PHYADDR_MASKL_64K) <<
                                     GITS_BASER_PHYADDR_OFFSETL_64K;
                c->dt.base_addr |= ((value >> GITS_BASER_PHYADDR_OFFSET) &
                                    GITS_BASER_PHYADDR_MASKH_64K) <<
                                     GITS_BASER_PHYADDR_OFFSETH_64K;
            }
        }
    } else if (type == GITS_ITT_TYPE_COLLECTION) {
        c->ct.valid = (value >> GITS_BASER_VALID) & GITS_BASER_VALID_MASK;

        /*
         * GITS_TYPER.HCC is 0 for this implementation
         * hence writes are discarded if ct.valid is 0
         */
        if (c->ct.valid) {
            c->ct.indirect = (value >> GITS_BASER_INDIRECT_OFFSET) &
                                       GITS_BASER_INDIRECT_MASK;
            c->ct.entry_sz = (value >> GITS_BASER_ENTRYSIZE_OFFSET) &
                                    GITS_BASER_ENTRYSIZE_MASK;

            if (!c->ct.indirect) {
                c->ct.max_entries = ((num_pages + 1) * page_sz) /
                                      c->ct.entry_sz;
            } else {
                c->ct.max_entries = ((((num_pages + 1) * page_sz) /
                                      L1TABLE_ENTRY_SIZE) *
                                      (page_sz / c->ct.entry_sz));
            }

            if ((value >> GITS_TYPER_CIL_OFFSET) & GITS_TYPER_CIL_MASK) {
                c->ct.max_collids = (1UL << (((value >>
                                               GITS_TYPER_CIDBITS_OFFSET) &
                                               GITS_TYPER_CIDBITS_MASK) + 1));
            } else {
                /* 16-bit CollectionId supported when CIL == 0 */
                c->ct.max_collids = (1UL << 16);
            }

            if ((page_sz == GITS_ITT_PAGE_SIZE_0) ||
                 (page_sz == GITS_ITT_PAGE_SIZE_1)) {
                c->ct.base_addr = (value >> GITS_BASER_PHYADDR_OFFSET) &
                                            GITS_BASER_PHYADDR_MASK;
                c->ct.base_addr <<= GITS_BASER_PHYADDR_OFFSET;
            } else if (page_sz == GITS_ITT_PAGE_SIZE_2) {
                c->ct.base_addr = ((value >> GITS_BASER_PHYADDR_OFFSETL_64K) &
                                   GITS_BASER_PHYADDR_MASKL_64K) <<
                                    GITS_BASER_PHYADDR_OFFSETL_64K;
                c->ct.base_addr |= ((value >> GITS_BASER_PHYADDR_OFFSET) &
                                    GITS_BASER_PHYADDR_MASKH_64K) <<
                                    GITS_BASER_PHYADDR_OFFSETH_64K;
            }
        }
    } else {
        /* unsupported ITS table type */
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Unsupported ITS table type %d",
                         __func__, type);
        return false;
    }
    return true;
}

static bool extract_cmdq_params(GICv3ITSState *s)
{
    GICv3ITSClass *c = ARM_GICV3_ITS_GET_CLASS(s);
    uint16_t num_pages = 0;
    uint64_t value = s->cbaser;

    num_pages = (value & GITS_CBASER_SIZE);

    c->cq.valid = (value >> GITS_CBASER_VALID_OFFSET) &
                                GITS_CBASER_VALID_MASK;

    if (!num_pages || !c->cq.valid) {
        return false;
    }

    if (c->cq.valid) {
        c->cq.max_entries = ((num_pages + 1) * GITS_ITT_PAGE_SIZE_0) /
                                                GITS_CMDQ_ENTRY_SIZE;
        c->cq.base_addr = (value >> GITS_CBASER_PHYADDR_OFFSET) &
                                    GITS_CBASER_PHYADDR_MASK;
        c->cq.base_addr <<= GITS_CBASER_PHYADDR_OFFSET;
    }
    return true;
}

static MemTxResult its_trans_writew(GICv3ITSState *s, hwaddr offset,
                               uint64_t value, MemTxAttrs attrs)
{
    MemTxResult result = MEMTX_OK;
    uint32_t devid = 0;

    switch (offset) {
    case GITS_TRANSLATER:
        if (s->ctlr & GITS_CTLR_ENABLED) {
            s->translater = (value & 0x0000FFFFU);
            devid = attrs.requester_id;
            result = process_int(s, s->translater, devid, NONE);
        }
        break;
    default:
        result = MEMTX_ERROR;
        break;
    }
    return result;
}

static MemTxResult its_trans_writel(GICv3ITSState *s, hwaddr offset,
                               uint64_t value, MemTxAttrs attrs)
{
    MemTxResult result = MEMTX_OK;
    uint32_t devid = 0;

    switch (offset) {
    case GITS_TRANSLATER:
        if (s->ctlr & GITS_CTLR_ENABLED) {
            s->translater = value;
            devid = attrs.requester_id;
            result = process_int(s, s->translater, devid, NONE);
        }
        break;
    default:
        result = MEMTX_ERROR;
        break;
    }
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
    int index;
    uint64_t temp = 0;

    switch (offset) {
    case GITS_CTLR:
        s->ctlr |= (value & ~(s->ctlr));
        break;
    case GITS_CBASER:
        /* GITS_CBASER register becomes RO if ITS is already enabled */
        if (!(s->ctlr & GITS_CTLR_ENABLED)) {
            s->cbaser = deposit64(s->cbaser, 0, 32, value);
            s->creadr = 0;
        }
        break;
    case GITS_CBASER + 4:
        /* GITS_CBASER register becomes RO if ITS is already enabled */
        if (!(s->ctlr & GITS_CTLR_ENABLED)) {
            s->cbaser = deposit64(s->cbaser, 32, 32, value);
            if (!extract_cmdq_params(s)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                       "%s: error extracting GITS_CBASER parameters "
                       TARGET_FMT_plx "\n", __func__, offset);
                s->cbaser = 0;
                result = MEMTX_ERROR;
            } else {
                s->creadr = 0;
            }
        }
        break;
    case GITS_CWRITER:
        s->cwriter = deposit64(s->cwriter, 0, 32, value);
        if ((s->ctlr & GITS_CTLR_ENABLED) && (s->cwriter != s->creadr)) {
            result = process_cmdq(s);
        }
        break;
    case GITS_CWRITER + 4:
        s->cwriter = deposit64(s->cwriter, 32, 32, value);
        break;
    case GITS_BASER ... GITS_BASER + 0x3f:
        /* GITS_BASERn registers become RO if ITS is already enabled */
        if (!(s->ctlr & GITS_CTLR_ENABLED)) {
            index = (offset - GITS_BASER) / 8;

            if (offset & 7) {
                temp = s->baser[index];
                temp = deposit64(temp, 32, 32, (value & ~GITS_BASER_VAL_MASK));
                s->baser[index] |= temp;

                if (!extract_table_params(s, index)) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                        "%s: error extracting GITS_BASER parameters "
                        TARGET_FMT_plx "\n", __func__, offset);
                    s->baser[index] = 0;
                    result = MEMTX_ERROR;
                }
            } else {
                s->baser[index] =  deposit64(s->baser[index], 0, 32, value);
            }
        }
        break;
    case GITS_IIDR:
    case GITS_TYPER:
    case GITS_CREADR:
        /* RO registers, ignore the write */
        qemu_log_mask(LOG_GUEST_ERROR,
            "%s: invalid guest write to RO register at offset "
            TARGET_FMT_plx "\n", __func__, offset);
        break;
    default:
        result = MEMTX_ERROR;
        break;
    }
    return result;
}

static MemTxResult its_readl(GICv3ITSState *s, hwaddr offset,
                               uint64_t *data, MemTxAttrs attrs)
{
    MemTxResult result = MEMTX_OK;
    int index;

    switch (offset) {
    case GITS_CTLR:
        *data = s->ctlr;
        break;
    case GITS_IIDR:
        *data = s->iidr;
        break;
    case GITS_PIDR2:
        *data = 0x30; /* GICv3 */
        break;
    case GITS_TYPER:
        *data = extract64(s->typer, 0, 32);
        break;
    case GITS_TYPER + 4:
        *data = extract64(s->typer, 32, 32);
        break;
    case GITS_CBASER:
        *data = extract64(s->cbaser, 0, 32);
        break;
    case GITS_CBASER + 4:
        *data = extract64(s->cbaser, 32, 32);
        break;
    case GITS_CREADR:
        *data = extract64(s->creadr, 0, 32);
        break;
    case GITS_CREADR + 4:
        *data = extract64(s->creadr, 32, 32);
        break;
    case GITS_CWRITER:
        *data = extract64(s->cwriter, 0, 32);
        break;
    case GITS_CWRITER + 4:
        *data = extract64(s->cwriter, 32, 32);
        break;
    case GITS_BASER ... GITS_BASER + 0x3f:
        index = (offset - GITS_BASER) / 8;
        if (offset & 7) {
            *data = s->baser[index] >> 32;
        } else {
            *data = (uint32_t)s->baser[index];
        }
        break;
    default:
        result = MEMTX_ERROR;
        break;
    }
    return result;
}

static MemTxResult its_writell(GICv3ITSState *s, hwaddr offset,
                               uint64_t value, MemTxAttrs attrs)
{
    MemTxResult result = MEMTX_OK;
    int index;

    switch (offset) {
    case GITS_BASER ... GITS_BASER + 0x3f:
        /* GITS_BASERn registers become RO if ITS is already enabled */
        if (!(s->ctlr & GITS_CTLR_ENABLED)) {
            index = (offset - GITS_BASER) / 8;
            s->baser[index] |= (value & ~GITS_BASER_VAL_MASK);
            if (!extract_table_params(s, index)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                        "%s: error extracting GITS_BASER parameters "
                        TARGET_FMT_plx "\n", __func__, offset);
                s->baser[index] = 0;
                result = MEMTX_ERROR;
            }
        }
        break;
    case GITS_CBASER:
        /* GITS_CBASER register becomes RO if ITS is already enabled */
        if (!(s->ctlr & GITS_CTLR_ENABLED)) {
            s->cbaser = value;
            if (!extract_cmdq_params(s)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                       "%s: error extracting GITS_CBASER parameters "
                       TARGET_FMT_plx "\n", __func__, offset);
                s->cbaser = 0;
                result = MEMTX_ERROR;
            } else {
                s->creadr = 0;
            }
        }
        break;
    case GITS_CWRITER:
        s->cwriter = value;
        if ((s->ctlr & GITS_CTLR_ENABLED) && (s->cwriter != s->creadr)) {
            result = process_cmdq(s);
        }
        break;
    case GITS_TYPER:
    case GITS_CREADR:
        /* RO register, ignore the write */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid guest write to RO register at offset "
                      TARGET_FMT_plx "\n", __func__, offset);
        break;
    default:
        result = MEMTX_ERROR;
        break;
    }
    return result;
}

static MemTxResult its_readll(GICv3ITSState *s, hwaddr offset,
                               uint64_t *data, MemTxAttrs attrs)
{
    MemTxResult result = MEMTX_OK;
    int index;

    switch (offset) {
    case GITS_TYPER:
        *data = s->typer;
        break;
    case GITS_BASER ... GITS_BASER + 0x3f:
        index = (offset - GITS_BASER) / 8;
        *data = s->baser[index];
        break;
    case GITS_CBASER:
        *data = s->cbaser;
        break;
    case GITS_CREADR:
        *data = s->creadr;
        break;
    case GITS_CWRITER:
        *data = s->cwriter;
        break;
    default:
        result = MEMTX_ERROR;
        break;
    }
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

    address_space_init(&s->gicv3->sysmem_as, s->gicv3->sysmem,
                        "gicv3-its-sysmem");
}

static void gicv3_its_reset(DeviceState *dev)
{
    GICv3ITSState *s = ARM_GICV3_ITS_COMMON(dev);
    GICv3ITSClass *c = ARM_GICV3_ITS_GET_CLASS(s);

    if (s->gicv3->cpu->gicr_typer & GICR_TYPER_PLPIS) {
        c->parent_reset(dev);
        memset(&c->dt, 0 , sizeof(c->dt));
        memset(&c->ct, 0 , sizeof(c->ct));
        memset(&c->cq, 0 , sizeof(c->cq));

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
