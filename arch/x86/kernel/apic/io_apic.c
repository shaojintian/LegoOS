/*
 * Copyright (c) 2016 Wuklab, Purdue University. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#define pr_fmt(fmt) "IO-APIC: " fmt

#include <asm/io.h>
#include <asm/apic.h>
#include <asm/numa.h>
#include <asm/i8259.h>
#include <asm/fixmap.h>
#include <asm/hw_irq.h>
#include <asm/io_apic.h>
#include <asm/irq_vectors.h>

#include <lego/irq.h>
#include <lego/irqdesc.h>
#include <lego/irqchip.h>
#include <lego/irqdomain.h>
#include <lego/slab.h>
#include <lego/time.h>
#include <lego/delay.h>
#include <lego/kernel.h>
#include <lego/string.h>
#include <lego/bitmap.h>
#include <lego/jiffies.h>
#include <lego/resource.h>
#include <lego/spinlock.h>

struct irq_pin_list {
	struct list_head		list;
	int 				apic;
	int				pin;
};

/*
 * This is the: desc->irq_data->chip_data for io-apic
 * The local apic has different chip_data
 */
struct mp_chip_data {
	struct list_head		irq_2_pin;
	struct IO_APIC_route_entry	entry;
	int				trigger;
	int				polarity;
	u32				count;
	bool				isa_irq;
};

#define	for_each_ioapic(idx)		\
	for ((idx) = 0; (idx) < nr_ioapics; (idx)++)
#define	for_each_ioapic_reverse(idx)	\
	for ((idx) = nr_ioapics - 1; (idx) >= 0; (idx)--)
#define	for_each_pin(idx, pin)		\
	for ((pin) = 0; (pin) < ioapics[(idx)].nr_registers; (pin)++)
#define	for_each_ioapic_pin(idx, pin)	\
	for_each_ioapic((idx))		\
		for_each_pin((idx), (pin))
#define for_each_irq_pin(entry, head) \
	list_for_each_entry(entry, &head, list)

static DEFINE_SPINLOCK(ioapic_lock);
static int ioapic_initialized;
static unsigned int ioapic_dynirq_base;

struct ioapic ioapics[MAX_IO_APICS];

/* The one past the highest gsi number used */
u32 gsi_top;

int nr_ioapics;

static inline struct irq_domain *mp_ioapic_irqdomain(int ioapic)
{
	return ioapics[ioapic].irqdomain;
}

static int find_free_ioapic_entry(void)
{
	int idx;

	for (idx = 0; idx < MAX_IO_APICS; idx++)
		if (ioapics[idx].nr_registers == 0)
			return idx;

	return MAX_IO_APICS;
}

int mpc_ioapic_id(int ioapic_idx)
{
	return ioapics[ioapic_idx].mp_config.apicid;
}

static inline struct mp_ioapic_gsi *mp_ioapic_gsi_routing(int ioapic_idx)
{
	return &ioapics[ioapic_idx].gsi_config;
}

static inline int mp_ioapic_pin_count(int ioapic)
{
	struct mp_ioapic_gsi *gsi_cfg = mp_ioapic_gsi_routing(ioapic);

	return gsi_cfg->gsi_end - gsi_cfg->gsi_base + 1;
}

static inline u32 mp_pin_to_gsi(int ioapic, int pin)
{
	return mp_ioapic_gsi_routing(ioapic)->gsi_base + pin;
}

static inline bool mp_is_legacy_irq(int irq)
{
	return irq >= 0 && irq < nr_legacy_irqs();
}

#define mpc_ioapic_ver(ioapic_idx)	ioapics[ioapic_idx].mp_config.apicver

unsigned int mpc_ioapic_addr(int ioapic_idx)
{
	return ioapics[ioapic_idx].mp_config.apicaddr;
}

struct io_apic {
	unsigned int index;
	unsigned int unused[3];
	unsigned int data;
	unsigned int unused2[11];
	unsigned int eoi;
};

static inline struct io_apic __iomem *io_apic_base(int idx)
{
	return (void __iomem *) __fix_to_virt(FIX_IO_APIC_BASE_0 + idx)
		+ (mpc_ioapic_addr(idx) & ~PAGE_MASK);
}

static inline unsigned int io_apic_read(unsigned int apic, unsigned int reg)
{
	struct io_apic __iomem *io_apic = io_apic_base(apic);
	writel(reg, &io_apic->index);
	return readl(&io_apic->data);
}

static inline void io_apic_write(unsigned int apic, unsigned int reg,
			  unsigned int value)
{
	struct io_apic __iomem *io_apic = io_apic_base(apic);

	writel(reg, &io_apic->index);
	writel(value, &io_apic->data);
}

union entry_union {
	struct { u32 w1, w2; };
	struct IO_APIC_route_entry entry;
};

static struct IO_APIC_route_entry __ioapic_read_entry(int apic, int pin)
{
	union entry_union eu;

	eu.w1 = io_apic_read(apic, 0x10 + 2 * pin);
	eu.w2 = io_apic_read(apic, 0x11 + 2 * pin);

	return eu.entry;
}

static struct IO_APIC_route_entry ioapic_read_entry(int apic, int pin)
{
	union entry_union eu;
	unsigned long flags;

	spin_lock_irqsave(&ioapic_lock, flags);
	eu.entry = __ioapic_read_entry(apic, pin);
	spin_unlock_irqrestore(&ioapic_lock, flags);

	return eu.entry;
}

/*
 * When we write a new IO APIC routing entry, we need to write the high
 * word first! If the mask bit in the low word is clear, we will enable
 * the interrupt, and we need to make sure the entry is fully populated
 * before that happens.
 */
static void __ioapic_write_entry(int apic, int pin, struct IO_APIC_route_entry e)
{
	union entry_union eu = {{0, 0}};

	eu.entry = e;
	io_apic_write(apic, 0x11 + 2*pin, eu.w2);
	io_apic_write(apic, 0x10 + 2*pin, eu.w1);
}

static void ioapic_write_entry(int apic, int pin, struct IO_APIC_route_entry e)
{
	unsigned long flags;

	spin_lock_irqsave(&ioapic_lock, flags);
	__ioapic_write_entry(apic, pin, e);
	spin_unlock_irqrestore(&ioapic_lock, flags);
}

/*
 * When we mask an IO APIC routing entry, we need to write the low
 * word first, in order to set the mask bit before we change the
 * high bits!
 */
static void ioapic_mask_entry(int apic, int pin)
{
	unsigned long flags;
	union entry_union eu = { .entry.mask = IOAPIC_MASKED };

	spin_lock_irqsave(&ioapic_lock, flags);
	io_apic_write(apic, 0x10 + 2*pin, eu.w1);
	io_apic_write(apic, 0x11 + 2*pin, eu.w2);
	spin_unlock_irqrestore(&ioapic_lock, flags);
}

static int bad_ioapic_register(int idx)
{
	union IO_APIC_reg_00 reg_00;
	union IO_APIC_reg_01 reg_01;
	union IO_APIC_reg_02 reg_02;

	reg_00.raw = io_apic_read(idx, 0);
	reg_01.raw = io_apic_read(idx, 1);
	reg_02.raw = io_apic_read(idx, 2);

	if (reg_00.raw == -1 && reg_01.raw == -1 && reg_02.raw == -1) {
		pr_warn("I/O APIC 0x%x registers return all ones, skipping!\n",
			mpc_ioapic_addr(idx));
		return 1;
	}

	return 0;
}

static inline void io_apic_eoi(unsigned int apic, unsigned int vector)
{
	struct io_apic __iomem *io_apic = io_apic_base(apic);
	writel(vector, &io_apic->eoi);
}

static u8 io_apic_unique_id(int idx, u8 id)
{
	union IO_APIC_reg_00 reg_00;
	DECLARE_BITMAP(used, 256);
	unsigned long flags;
	u8 new_id;
	int i;

	bitmap_zero(used, 256);
	for_each_ioapic(i)
		__set_bit(mpc_ioapic_id(i), used);

	/* Hand out the requested id if available */
	if (!test_bit(id, used))
		return id;

	/*
	 * Read the current id from the ioapic and keep it if
	 * available.
	 */
	spin_lock_irqsave(&ioapic_lock, flags);
	reg_00.raw = io_apic_read(idx, 0);
	spin_unlock_irqrestore(&ioapic_lock, flags);

	new_id = reg_00.bits.ID;
	if (!test_bit(new_id, used)) {
		pr_debug("IOAPIC[%d]: Using reg apic_id %d instead of %d\n",
			 idx, new_id, id);
		return new_id;
	}

	/*
	 * Get the next free id and write it to the ioapic.
	 */
	new_id = find_first_zero_bit(used, 256);
	reg_00.bits.ID = new_id;

	spin_lock_irqsave(&ioapic_lock, flags);
	io_apic_write(idx, 0, reg_00.raw);
	reg_00.raw = io_apic_read(idx, 0);
	spin_unlock_irqrestore(&ioapic_lock, flags);

	/* Sanity check */
	BUG_ON(reg_00.bits.ID != new_id);

	return new_id;
}

static int io_apic_get_version(int ioapic)
{
	union IO_APIC_reg_01	reg_01;
	unsigned long flags;

	spin_lock_irqsave(&ioapic_lock, flags);
	reg_01.raw = io_apic_read(ioapic, 1);
	spin_unlock_irqrestore(&ioapic_lock, flags);

	return reg_01.bits.version;
}

static int io_apic_get_redir_entries(int ioapic)
{
	union IO_APIC_reg_01	reg_01;
	unsigned long flags;

	spin_lock_irqsave(&ioapic_lock, flags);
	reg_01.raw = io_apic_read(ioapic, 1);
	spin_unlock_irqrestore(&ioapic_lock, flags);

	/*
	 * The register returns the maximum index redir index
	 * supported, which is one less than the total number
	 * of redir entries.
	 */
	return reg_01.bits.entries + 1;
}

int mp_find_ioapic(u32 gsi)
{
	int i;

	if (nr_ioapics == 0)
		return -1;

	/* Find the IOAPIC that manages this GSI. */
	for_each_ioapic(i) {
		struct mp_ioapic_gsi *gsi_cfg = mp_ioapic_gsi_routing(i);
		if (gsi >= gsi_cfg->gsi_base && gsi <= gsi_cfg->gsi_end)
			return i;
	}

	pr_err("ERROR: Unable to locate IOAPIC for GSI %d\n", gsi);
	return -1;
}

int mp_find_ioapic_pin(int ioapic, u32 gsi)
{
	struct mp_ioapic_gsi *gsi_cfg;

	if (WARN_ON(ioapic < 0))
		return -1;

	gsi_cfg = mp_ioapic_gsi_routing(ioapic);
	if (WARN_ON(gsi > gsi_cfg->gsi_end))
		return -1;

	return gsi - gsi_cfg->gsi_base;
}

static void alloc_ioapic_saved_registers(int idx)
{
	size_t size;

	if (ioapics[idx].saved_registers)
		return;

	size = sizeof(struct IO_APIC_route_entry) * ioapics[idx].nr_registers;
	ioapics[idx].saved_registers = kzalloc(size, GFP_KERNEL);
	if (!ioapics[idx].saved_registers)
		pr_err("IOAPIC %d: suspend/resume impossible!\n", idx);
}

static void __used free_ioapic_saved_registers(int idx)
{
	kfree(ioapics[idx].saved_registers);
	ioapics[idx].saved_registers = NULL;
}

/**
 * mp_register_ioapic - Register an IOAPIC device
 * @id:		hardware IOAPIC ID
 * @address:	physical address of IOAPIC register area
 * @gsi_base:	base of GSI associated with the IOAPIC
 * @cfg:	configuration information for the IOAPIC
 */
int mp_register_ioapic(int id, u32 address, u32 gsi_base, struct ioapic_domain_cfg *cfg)
{
	bool hotplug = !!ioapic_initialized;
	struct mp_ioapic_gsi *gsi_cfg;
	int idx, ioapic, entries;
	u32 gsi_end;

	if (!address) {
		pr_warn("Bogus (zero) I/O APIC address found, skipping!\n");
		return -EINVAL;
	}

	for_each_ioapic(ioapic)
		if (ioapics[ioapic].mp_config.apicaddr == address) {
			pr_warn("address 0x%x conflicts with IOAPIC%d\n",
				address, ioapic);
			return -EEXIST;
		}

	idx = find_free_ioapic_entry();
	if (idx >= MAX_IO_APICS) {
		pr_warn("Max # of I/O APICs (%d) exceeded (found %d), skipping\n",
			MAX_IO_APICS, idx);
		return -ENOSPC;
	}

	ioapics[idx].mp_config.type = MP_IOAPIC;
	ioapics[idx].mp_config.flags = MPC_APIC_USABLE;
	ioapics[idx].mp_config.apicaddr = address;

	set_fixmap_nocache(FIX_IO_APIC_BASE_0 + idx, address);
	if (bad_ioapic_register(idx)) {
		clear_fixmap(FIX_IO_APIC_BASE_0 + idx);
		return -ENODEV;
	}

	ioapics[idx].mp_config.apicid = io_apic_unique_id(idx, id);
	ioapics[idx].mp_config.apicver = io_apic_get_version(idx);

	/*
	 * Build basic GSI lookup table to facilitate gsi->io_apic lookups
	 * and to prevent reprogramming of IOAPIC pins (PCI GSIs).
	 */
	entries = io_apic_get_redir_entries(idx);
	gsi_end = gsi_base + entries - 1;
	for_each_ioapic(ioapic) {
		gsi_cfg = mp_ioapic_gsi_routing(ioapic);
		if ((gsi_base >= gsi_cfg->gsi_base &&
		     gsi_base <= gsi_cfg->gsi_end) ||
		    (gsi_end >= gsi_cfg->gsi_base &&
		     gsi_end <= gsi_cfg->gsi_end)) {
			pr_warn("GSI range [%u-%u] for new IOAPIC conflicts with GSI[%u-%u]\n",
				gsi_base, gsi_end,
				gsi_cfg->gsi_base, gsi_cfg->gsi_end);
			clear_fixmap(FIX_IO_APIC_BASE_0 + idx);
			return -ENOSPC;
		}
	}
	gsi_cfg = mp_ioapic_gsi_routing(idx);
	gsi_cfg->gsi_base = gsi_base;
	gsi_cfg->gsi_end = gsi_end;

	/*
	 * If mp_register_ioapic() is called during early boot stage when
	 * walking ACPI/SFI/DT tables, it's too early to create irqdomain,
	 * we are still using bootmem allocator. So delay it to setup_IO_APIC().
	 */
	if (hotplug)
		alloc_ioapic_saved_registers(idx);

	if (gsi_cfg->gsi_end >= gsi_top)
		gsi_top = gsi_cfg->gsi_end + 1;
	if (nr_ioapics <= idx)
		nr_ioapics = idx + 1;

	/* Set nr_registers to mark entry present */
	ioapics[idx].nr_registers = entries;

	ioapics[idx].irqdomain_cfg = *cfg;

	pr_info("IOAPIC[%d]: apic_id %d, version %d, address 0x%x, nr_redir_entries %d, GSI %d-%d\n",
		idx, mpc_ioapic_id(idx),
		mpc_ioapic_ver(idx), mpc_ioapic_addr(idx), entries,
		gsi_cfg->gsi_base, gsi_cfg->gsi_end);

	return 0;
}

void __init arch_ioapic_init(void)
{
	int i;

	if (!nr_legacy_irqs())
		io_apic_irqs = ~0UL;

	for_each_ioapic(i)
		alloc_ioapic_saved_registers(i);
}

static void io_apic_modify_irq(struct mp_chip_data *data,
			       int mask_and, int mask_or,
			       void (*final)(struct irq_pin_list *entry))
{
	union entry_union eu;
	struct irq_pin_list *entry;

	eu.entry = data->entry;
	eu.w1 &= mask_and;
	eu.w1 |= mask_or;
	data->entry = eu.entry;

	for_each_irq_pin(entry, data->irq_2_pin) {
		io_apic_write(entry->apic, 0x10 + 2 * entry->pin, eu.w1);
		if (final)
			final(entry);
	}
}

static void __unmask_ioapic(struct mp_chip_data *data)
{
	io_apic_modify_irq(data, ~IO_APIC_REDIR_MASKED, 0, NULL);
}

static void unmask_ioapic_irq(struct irq_data *irq_data)
{
	struct mp_chip_data *data = irq_data->chip_data;
	unsigned long flags;

	spin_lock_irqsave(&ioapic_lock, flags);
	__unmask_ioapic(data);
	spin_unlock_irqrestore(&ioapic_lock, flags);
}

/*
 * IO-APIC versions below 0x20 don't support EOI register.
 * For the record, here is the information about various versions:
 *     0Xh     82489DX
 *     1Xh     I/OAPIC or I/O(x)APIC which are not PCI 2.2 Compliant
 *     2Xh     I/O(x)APIC which is PCI 2.2 Compliant
 *     30h-FFh Reserved
 *
 * Some of the Intel ICH Specs (ICH2 to ICH5) documents the io-apic
 * version as 0x2. This is an error with documentation and these ICH chips
 * use io-apic's of version 0x20.
 *
 * For IO-APIC's with EOI register, we use that to do an explicit EOI.
 * Otherwise, we simulate the EOI message manually by changing the trigger
 * mode to edge and then back to level, with RTE being masked during this.
 */
static void __eoi_ioapic_pin(int apic, int pin, int vector)
{
	if (mpc_ioapic_ver(apic) >= 0x20) {
		io_apic_eoi(apic, vector);
	} else {
		struct IO_APIC_route_entry entry, entry1;

		entry = entry1 = __ioapic_read_entry(apic, pin);

		/*
		 * Mask the entry and change the trigger mode to edge.
		 */
		entry1.mask = IOAPIC_MASKED;
		entry1.trigger = IOAPIC_EDGE;

		__ioapic_write_entry(apic, pin, entry1);

		/*
		 * Restore the previous level triggered entry.
		 */
		__ioapic_write_entry(apic, pin, entry);
	}
}

static void clear_IO_APIC_pin(unsigned int apic, unsigned int pin)
{
	struct IO_APIC_route_entry entry;

	/* Check delivery_mode to be sure we're not clearing an SMI pin */
	entry = ioapic_read_entry(apic, pin);
	if (entry.delivery_mode == dest_SMI)
		return;

	/*
	 * Make sure the entry is masked and re-read the contents to check
	 * if it is a level triggered pin and if the remote-IRR is set.
	 */
	if (entry.mask == IOAPIC_UNMASKED) {
		entry.mask = IOAPIC_MASKED;
		ioapic_write_entry(apic, pin, entry);
		entry = ioapic_read_entry(apic, pin);
	}

	if (entry.irr) {
		unsigned long flags;

		/*
		 * Make sure the trigger mode is set to level. Explicit EOI
		 * doesn't clear the remote-IRR if the trigger mode is not
		 * set to level.
		 */
		if (entry.trigger == IOAPIC_EDGE) {
			entry.trigger = IOAPIC_LEVEL;
			ioapic_write_entry(apic, pin, entry);
		}
		spin_lock_irqsave(&ioapic_lock, flags);
		__eoi_ioapic_pin(apic, pin, entry.vector);
		spin_unlock_irqrestore(&ioapic_lock, flags);
	}

	/*
	 * Clear the rest of the bits in the IO-APIC RTE except for the mask
	 * bit.
	 */
	ioapic_mask_entry(apic, pin);
	entry = ioapic_read_entry(apic, pin);
	if (entry.irr)
		pr_err("Unable to reset IRR for apic: %d, pin :%d\n",
		       mpc_ioapic_id(apic), pin);
}

static void clear_IO_APIC (void)
{
	int apic, pin;

	for_each_ioapic_pin(apic, pin)
		clear_IO_APIC_pin(apic, pin);
}

#define MP_APIC_ALL	0xFF

/* MP IRQ source entries */
struct mpc_intsrc mp_irqs[MAX_IRQ_SOURCES];

/* # of MP IRQ source entries */
int mp_irq_entries;

DECLARE_BITMAP(mp_bus_not_pci, MAX_MP_BUSSES);

/* Will be called in mpparse/acpi/sfi codes for saving IRQ info */
void mp_save_irq(struct mpc_intsrc *m)
{
	int i;

	pr_debug("Int: type %d, pol %d, trig %d, bus %02x,"
		" IRQ %02x, APIC ID %x, APIC INT %02x\n",
		m->irqtype, m->irqflag & 3, (m->irqflag >> 2) & 3, m->srcbus,
		m->srcbusirq, m->dstapic, m->dstirq);

	for (i = 0; i < mp_irq_entries; i++) {
		if (!memcmp(&mp_irqs[i], m, sizeof(*m)))
			return;
	}

	memcpy(&mp_irqs[mp_irq_entries], m, sizeof(*m));
	if (++mp_irq_entries == MAX_IRQ_SOURCES)
		panic("Max # of irq sources exceeded!!\n");
}

/*
 * Find the pin to which IRQ[irq] (ISA) is connected
 */
static int __init find_isa_irq_pin(int irq, int type)
{
	int i;

	for (i = 0; i < mp_irq_entries; i++) {
		int lbus = mp_irqs[i].srcbus;

		if (test_bit(lbus, mp_bus_not_pci) &&
		    (mp_irqs[i].irqtype == type) &&
		    (mp_irqs[i].srcbusirq == irq))

			return mp_irqs[i].dstirq;
	}
	return -1;
}

static int __init find_isa_irq_apic(int irq, int type)
{
	int i;

	for (i = 0; i < mp_irq_entries; i++) {
		int lbus = mp_irqs[i].srcbus;

		if (test_bit(lbus, mp_bus_not_pci) &&
		    (mp_irqs[i].irqtype == type) &&
		    (mp_irqs[i].srcbusirq == irq))
			break;
	}

	if (i < mp_irq_entries) {
		int ioapic_idx;

		for_each_ioapic(ioapic_idx)
			if (mpc_ioapic_id(ioapic_idx) == mp_irqs[i].dstapic)
				return ioapic_idx;
	}

	return -1;
}

/*
 * Find the IRQ entry number of a certain pin.
 */
static int find_irq_entry(int ioapic_idx, int pin, int type)
{
	int i;

	for (i = 0; i < mp_irq_entries; i++)
		if (mp_irqs[i].irqtype == type &&
		    (mp_irqs[i].dstapic == mpc_ioapic_id(ioapic_idx) ||
		     mp_irqs[i].dstapic == MP_APIC_ALL) &&
		    mp_irqs[i].dstirq == pin)
			return i;

	return -1;
}

/* Where if anywhere is the i8259 connect in external int mode */
static struct { int pin, apic; } ioapic_i8259 = { -1, -1 };

/* This is not real enable.. just check the ioapic i8259 things */
void __init enable_IO_APIC(void)
{
	int i8259_apic, i8259_pin;
	int apic, pin;

	if (!nr_legacy_irqs() || !nr_ioapics)
		return;

	for_each_ioapic_pin(apic, pin) {
		/* See if any of the pins is in ExtINT mode */
		struct IO_APIC_route_entry entry = ioapic_read_entry(apic, pin);

		/* If the interrupt line is enabled and in ExtInt mode
		 * I have found the pin where the i8259 is connected.
		 */
		if ((entry.mask == 0) && (entry.delivery_mode == dest_ExtINT)) {
			ioapic_i8259.apic = apic;
			ioapic_i8259.pin  = pin;
			goto found_i8259;
		}
	}
 found_i8259:
	/* Look to see what if the MP table has reported the ExtINT */
	/* If we could not find the appropriate pin by looking at the ioapic
	 * the i8259 probably is not connected the ioapic but give the
	 * mptable a chance anyway.
	 */
	i8259_pin  = find_isa_irq_pin(0, mp_ExtINT);
	i8259_apic = find_isa_irq_apic(0, mp_ExtINT);
	/* Trust the MP table if nothing is setup in the hardware */
	if ((ioapic_i8259.pin == -1) && (i8259_pin >= 0)) {
		pr_warn("ExtINT not setup in hardware but reported by MP table\n");
		ioapic_i8259.pin  = i8259_pin;
		ioapic_i8259.apic = i8259_apic;
	}
	/* Complain if the MP table and the hardware disagree */
	if (((ioapic_i8259.apic != i8259_apic) || (ioapic_i8259.pin != i8259_pin)) &&
		(i8259_pin >= 0) && (ioapic_i8259.pin >= 0)) {
		pr_warn("ExtINT in hardware and MP table differ\n");
	}

	/*
	 * Do not trust the IO-APIC being empty at bootup
	 */
	clear_IO_APIC();
}

/* ISA interrupts are always active high edge triggered,
 * when listed as conforming in the MP table. */

#define default_ISA_trigger(idx)	(IOAPIC_EDGE)
#define default_ISA_polarity(idx)	(IOAPIC_POL_HIGH)

/* EISA interrupts are always polarity zero and can be edge or level
 * trigger depending on the ELCR value.  If an interrupt is listed as
 * EISA conforming in the MP table, that means its trigger type must
 * be read in from the ELCR */

#define default_EISA_trigger(idx)	(EISA_ELCR(mp_irqs[idx].srcbusirq))
#define default_EISA_polarity(idx)	default_ISA_polarity(idx)

/* PCI interrupts are always active low level triggered,
 * when listed as conforming in the MP table. */

#define default_PCI_trigger(idx)	(IOAPIC_LEVEL)
#define default_PCI_polarity(idx)	(IOAPIC_POL_LOW)

static int irq_trigger(int idx)
{
	int bus = mp_irqs[idx].srcbus;
	int trigger;

	/*
	 * Determine IRQ trigger mode (edge or level sensitive):
	 */
	switch ((mp_irqs[idx].irqflag >> 2) & 0x03) {
	case 0:
		/* conforms to spec, ie. bus-type dependent trigger mode */
		if (test_bit(bus, mp_bus_not_pci))
			trigger = default_ISA_trigger(idx);
		else
			trigger = default_PCI_trigger(idx);
		return trigger;
	case 1:
		return IOAPIC_EDGE;
	case 2:
		pr_warn("IOAPIC: Invalid trigger mode 2 defaulting to level\n");
	case 3:
	default: /* Pointless default required due to do gcc stupidity */
		return IOAPIC_LEVEL;
	}
}

static int irq_polarity(int idx)
{
	int bus = mp_irqs[idx].srcbus;

	/*
	 * Determine IRQ line polarity (high active or low active):
	 */
	switch (mp_irqs[idx].irqflag & 0x03) {
	case 0:
		/* conforms to spec, ie. bus-type dependent polarity */
		if (test_bit(bus, mp_bus_not_pci))
			return default_ISA_polarity(idx);
		else
			return default_PCI_polarity(idx);
	case 1:
		return IOAPIC_POL_HIGH;
	case 2:
		pr_warn("IOAPIC: Invalid polarity: 2, defaulting to low\n");
	case 3:
	default: /* Pointless default required due to do gcc stupidity */
		return IOAPIC_POL_LOW;
	}
}

int acpi_get_override_irq(u32 gsi, int *trigger, int *polarity)
{
	int ioapic, pin, idx;

	ioapic = mp_find_ioapic(gsi);
	if (ioapic < 0)
		return -1;

	pin = mp_find_ioapic_pin(ioapic, gsi);
	if (pin < 0)
		return -1;

	idx = find_irq_entry(ioapic, pin, mp_INT);
	if (idx < 0)
		return -1;

	*trigger = irq_trigger(idx);
	*polarity = irq_polarity(idx);
	return 0;
}

static void copy_irq_alloc_info(struct irq_alloc_info *dst, struct irq_alloc_info *src)
{
	if (src)
		*dst = *src;
	else
		memset(dst, 0, sizeof(*dst));
}

static void ioapic_copy_alloc_attr(struct irq_alloc_info *dst,
				   struct irq_alloc_info *src,
				   u32 gsi, int ioapic_idx, int pin)
{
	int trigger, polarity;

	copy_irq_alloc_info(dst, src);
	dst->type = X86_IRQ_ALLOC_TYPE_IOAPIC;
	dst->ioapic_id = mpc_ioapic_id(ioapic_idx);
	dst->ioapic_pin = pin;
	dst->ioapic_valid = 1;
	if (src && src->ioapic_valid) {
		dst->ioapic_node = src->ioapic_node;
		dst->ioapic_trigger = src->ioapic_trigger;
		dst->ioapic_polarity = src->ioapic_polarity;
	} else {
		dst->ioapic_node = NUMA_NO_NODE;
		if (acpi_get_override_irq(gsi, &trigger, &polarity) >= 0) {
			dst->ioapic_trigger = trigger;
			dst->ioapic_polarity = polarity;
		} else {
			/*
			 * PCI interrupts are always active low level
			 * triggered.
			 */
			dst->ioapic_trigger = IOAPIC_LEVEL;
			dst->ioapic_polarity = IOAPIC_POL_LOW;
		}
	}
}

static void mp_register_handler(unsigned int irq, unsigned long trigger)
{
}

static bool mp_check_pin_attr(int irq, struct irq_alloc_info *info)
{
	struct mp_chip_data *data = irq_get_chip_data(irq);

	/*
	 * setup_IO_APIC_irqs() programs all legacy IRQs with default trigger
	 * and polarity attirbutes. So allow the first user to reprogram the
	 * pin with real trigger and polarity attributes.
	 */
	if (irq < nr_legacy_irqs() && data->count == 1) {
		if (info->ioapic_trigger != data->trigger)
			mp_register_handler(irq, info->ioapic_trigger);
		data->entry.trigger = data->trigger = info->ioapic_trigger;
		data->entry.polarity = data->polarity = info->ioapic_polarity;
	}

	return data->trigger == info->ioapic_trigger &&
	       data->polarity == info->ioapic_polarity;
}

/*
 * The common case is 1:1 IRQ<->pin mappings. Sometimes there are
 * shared ISA-space IRQs, so we have to support them. We are super
 * fast in the common case, and fast for shared ISA-space IRQs.
 *
 * Return 0 if there is already the same pin in the list or we
 * inserted ths new pin successfully.
 */
static int __add_pin_to_irq_node(struct mp_chip_data *data,
				 int node, int apic, int pin)
{
	struct irq_pin_list *entry;

	/* Don't allow duplicates */
	list_for_each_entry(entry, &data->irq_2_pin, list)
		if (entry->apic == apic && entry->pin == pin)
			return 0;

	entry = kzalloc_node(sizeof(struct irq_pin_list), GFP_KERNEL, node);
	if (!entry) {
		pr_err("can not alloc irq_pin_list (%d,%d,%d)\n",
		       node, apic, pin);
		return -ENOMEM;
	}
	entry->apic = apic;
	entry->pin = pin;
	list_add_tail(&entry->list, &data->irq_2_pin);

	return 0;
}

static void add_pin_to_irq_node(struct mp_chip_data *data,
				int node, int apic, int pin)
{
	if (__add_pin_to_irq_node(data, node, apic, pin))
		panic("IO-APIC: failed to add irq-pin. Can not proceed\n");
}

static int alloc_irq_from_domain(struct irq_domain *domain, int ioapic, u32 gsi,
				 struct irq_alloc_info *info)
{
	return 0;
}

static int ioapic_alloc_attr_node(struct irq_alloc_info *info)
{
	return (info && info->ioapic_valid) ? info->ioapic_node : NUMA_NO_NODE;
}

/*
 * Need special handling for ISA IRQs because there may be multiple IOAPIC pins
 * sharing the same ISA IRQ number and irqdomain only supports 1:1 mapping
 * between IOAPIC pin and IRQ number. A typical IOAPIC has 24 pins, pin 0-15 are
 * used for legacy IRQs and pin 16-23 are used for PCI IRQs (PIRQ A-H).
 * When ACPI is disabled, only legacy IRQ numbers (IRQ0-15) are available, and
 * some BIOSes may use MP Interrupt Source records to override IRQ numbers for
 * PIRQs instead of reprogramming the interrupt routing logic. Thus there may be
 * multiple pins sharing the same legacy IRQ number when ACPI is disabled.
 */
static int alloc_isa_irq_from_domain(struct irq_domain *domain,
				     int irq, int ioapic, int pin,
				     struct irq_alloc_info *info)
{
	struct mp_chip_data *data;
	struct irq_data *irq_data = irq_get_irq_data(irq);
	int node = ioapic_alloc_attr_node(info);

	/*
	 * Legacy ISA IRQ has already been allocated, just add pin to
	 * the pin list assoicated with this IRQ and program the IOAPIC
	 * entry. The IOAPIC entry
	 */
	if (irq_data && irq_data->parent_data) {
		if (!mp_check_pin_attr(irq, info))
			return -EBUSY;
		if (__add_pin_to_irq_node(irq_data->chip_data, node, ioapic,
					  info->ioapic_pin))
			return -ENOMEM;
	} else {
		irq = __irq_domain_alloc_irqs(domain, irq, 1, node, info, true, NULL);
		if (irq >= 0) {
			data = irq_data->chip_data;
			data->isa_irq = true;
		}
	}

	return irq;
}

static int mp_map_pin_to_irq(u32 gsi, int idx, int ioapic, int pin,
			     unsigned int flags, struct irq_alloc_info *info)
{
	int irq;
	bool legacy = false;
	struct irq_alloc_info tmp;
	struct mp_chip_data *data;
	struct irq_domain *domain = mp_ioapic_irqdomain(ioapic);

	if (!domain)
		return -ENOSYS;

	if (idx >= 0 && test_bit(mp_irqs[idx].srcbus, mp_bus_not_pci)) {
		irq = mp_irqs[idx].srcbusirq;
		legacy = mp_is_legacy_irq(irq);
	}

	spin_lock(&ioapic_lock);
	if (!(flags & IOAPIC_MAP_ALLOC)) {
		if (!legacy) {
			irq = irq_find_mapping(domain, pin);
			if (irq == 0)
				irq = -ENOENT;
		}
	} else {
		ioapic_copy_alloc_attr(&tmp, info, gsi, ioapic, pin);
		if (legacy)
			irq = alloc_isa_irq_from_domain(domain, irq,
							ioapic, pin, &tmp);
		else if ((irq = irq_find_mapping(domain, pin)) == 0)
			irq = alloc_irq_from_domain(domain, ioapic, gsi, &tmp);
		else if (!mp_check_pin_attr(irq, &tmp))
			irq = -EBUSY;
		if (irq >= 0) {
			data = irq_get_chip_data(irq);
			data->count++;
		}
	}
	spin_unlock(&ioapic_lock);

	return irq;
}

/**
 * pin_2_irq		-	Map from the pin number to IRQ number
 *
 * @idx:	Index of into the IRETENTRY
 * @ioapic:	Index of the ioapin
 * @pin:	pin number
 * @flags:	allocate or not
 *
 * Return:	Lego IRQ number
 */
static int pin_2_irq(int idx, int ioapic, int pin, unsigned int flags)
{
	u32 gsi = mp_pin_to_gsi(ioapic, pin);
	int irq;

	/*
	 * Debugging check, we are in big trouble if this message pops up!
	 */
	if (mp_irqs[idx].dstirq != pin)
		pr_err("broken BIOS or MPTABLE parser, ayiee!!\n");

	irq = mp_map_pin_to_irq(gsi, idx, ioapic, pin, flags, NULL);
	return irq;
}

static void __init setup_IO_APIC_irqs(void)
{
	unsigned int ioapic, pin;
	int idx;

	apic_printk(APIC_VERBOSE, KERN_DEBUG "init IO_APIC IRQs\n");

	for_each_ioapic_pin(ioapic, pin) {
		idx = find_irq_entry(ioapic, pin, mp_INT);
		if (idx < 0) {
			apic_printk(APIC_VERBOSE,
				" ioapic[%d] apic %d pin: %d not connected\n",
				ioapic, mpc_ioapic_id(ioapic), pin);
			continue;
		}

		pin_2_irq(idx, ioapic, pin, ioapic ? 0 : IOAPIC_MAP_ALLOC);
	}
}

/*
 * Traditionally ISA IRQ2 is the cascade IRQ, and is not available
 * to devices.  However there may be an I/O APIC pin available for
 * this interrupt regardless.  The pin may be left unconnected, but
 * typically it will be reused as an ExtINT cascade interrupt for
 * the master 8259A.  In the MPS case such a pin will normally be
 * reported as an ExtINT interrupt in the MP table.  With ACPI
 * there is no provision for ExtINT interrupts, and in the absence
 * of an override it would be treated as an ordinary ISA I/O APIC
 * interrupt, that is edge-triggered and unmasked by default.  We
 * used to do this, but it caused problems on some systems because
 * of the NMI watchdog and sometimes IRQ0 of the 8254 timer using
 * the same ExtINT cascade interrupt to drive the local APIC of the
 * bootstrap processor.  Therefore we refrain from routing IRQ2 to
 * the I/O APIC in all cases now.  No actual device should request
 * it anyway.  --macro
 */
#define PIC_IRQS	(1UL << PIC_CASCADE_IR)

unsigned int arch_dynirq_lower_bound(unsigned int from)
{
	/*
	 * dmar_alloc_hwirq() may be called before setup_IO_APIC(), so use
	 * gsi_top if ioapic_dynirq_base hasn't been initialized yet.
	 */
	return ioapic_initialized ? ioapic_dynirq_base : gsi_top;
}

int mp_irqdomain_ioapic_idx(struct irq_domain *domain)
{
	return (int)(long)domain->host_data;
}

static inline void init_IO_APIC_traps(void)
{
	struct irq_cfg *cfg;
	unsigned int irq;

	for_each_active_irq(irq) {
		cfg = irq_cfg(irq);
		if (IO_APIC_IRQ(irq) && cfg && !cfg->vector) {
			/*
			 * Hmm.. We don't have an entry for this,
			 * so default to an old-fashioned 8259
			 * interrupt if we can..
			 */
			if (irq < nr_legacy_irqs())
				legacy_pic->make_irq(irq);
			else
				/* Strange. Oh, well.. */
				irq_set_chip(irq, &no_irq_chip);
		}
	}
}

void init_irq_alloc_info(struct irq_alloc_info *info,
			 const struct cpumask *mask)
{
	memset(info, 0, sizeof(*info));
	info->mask = mask;
}

void ioapic_set_alloc_attr(struct irq_alloc_info *info, int node,
			   int trigger, int polarity)
{
	init_irq_alloc_info(info, NULL);
	info->type = X86_IRQ_ALLOC_TYPE_IOAPIC;
	info->ioapic_node = node;
	info->ioapic_trigger = trigger;
	info->ioapic_polarity = polarity;
	info->ioapic_valid = 1;
}

static int mp_alloc_timer_irq(int ioapic, int pin)
{
	int irq = -1;
	struct irq_domain *domain = mp_ioapic_irqdomain(ioapic);

	if (domain) {
		struct irq_alloc_info info;

		ioapic_set_alloc_attr(&info, NUMA_NO_NODE, 0, 0);
		info.ioapic_id = mpc_ioapic_id(ioapic);
		info.ioapic_pin = pin;

		spin_lock(&ioapic_lock);
		irq = alloc_isa_irq_from_domain(domain, 0, ioapic, pin, &info);
		spin_unlock(&ioapic_lock);
	}

	return irq;
}

/*
 * There is a nasty bug in some older SMP boards, their mptable lies
 * about the timer IRQ. We do the following to work around the situation:
 *
 *	- timer IRQ defaults to IO-APIC IRQ
 *	- if this function detects that timer IRQs are defunct, then we fall
 *	  back to ISA timer IRQs
 */
static int __init timer_irq_works(void)
{
	unsigned long t1 = jiffies;
	unsigned long flags;

	local_save_flags(flags);
	local_irq_enable();
	/* Let ten ticks pass... */
	mdelay((10 * 1000) / HZ);
	local_irq_restore(flags);

	/*
	 * Expect a few ticks at least, to be sure some possible
	 * glue logic does not lock up after one or two first
	 * ticks in a non-ExtINT mode.  Also the local APIC
	 * might have cached one ExtINT interrupt.  Finally, at
	 * least one tick may be lost due to delays.
	 */

	/* jiffies wrap? */
	if (time_after(jiffies, t1 + 4))
		return 1;
	return 1;
}

/*
 * Reroute an IRQ to a different pin.
 */
static void __init replace_pin_at_irq_node(struct mp_chip_data *data, int node,
					   int oldapic, int oldpin,
					   int newapic, int newpin)
{
	struct irq_pin_list *entry;

	for_each_irq_pin(entry, data->irq_2_pin) {
		if (entry->apic == oldapic && entry->pin == oldpin) {
			entry->apic = newapic;
			entry->pin = newpin;
			/* every one is different, right? */
			return;
		}
	}

	/* old apic/pin didn't exist, so just add new ones */
	add_pin_to_irq_node(data, node, newapic, newpin);
}

/*
 * This code may look a bit paranoid, but it's supposed to cooperate with
 * a wide range of boards and BIOS bugs.  Fortunately only the timer IRQ
 * is so screwy.  Thanks to Brian Perkins for testing/hacking this beast
 * fanatically on his truly buggy board.
 *
 * FIXME: really need to revamp this for all platforms.
 */
static inline void __init check_timer(void)
{
	struct irq_data *irq_data = irq_get_irq_data(0);
	struct mp_chip_data *data = irq_data->chip_data;
	struct irq_cfg *cfg = irqd_cfg(irq_data);
	int node = cpu_to_node(0);
	int apic1, pin1, apic2, pin2;
	unsigned long flags;
	int no_pin1 = 0;

	local_irq_save(flags);

	/*
	 * get/set the timer IRQ vector:
	 */
	legacy_pic->mask(0);

	/*
	 * As IRQ0 is to be enabled in the 8259A, the virtual
	 * wire has to be disabled in the local APIC.  Also
	 * timer interrupts need to be acknowledged manually in
	 * the 8259A for the i82489DX when using the NMI
	 * watchdog as that APIC treats NMIs as level-triggered.
	 * The AEOI mode will finish them in the 8259A
	 * automatically.
	 */
	apic_write(APIC_LVT0, APIC_LVT_MASKED | APIC_DM_EXTINT);
	legacy_pic->init(1);

	pin1  = find_isa_irq_pin(0, mp_INT);
	apic1 = find_isa_irq_apic(0, mp_INT);
	pin2  = ioapic_i8259.pin;
	apic2 = ioapic_i8259.apic;

	apic_printk(APIC_QUIET, "..TIMER: vector=0x%02X "
		    "apic1=%d pin1=%d apic2=%d pin2=%d\n",
		    cfg->vector, apic1, pin1, apic2, pin2);

	/*
	 * Some BIOS writers are clueless and report the ExtINTA
	 * I/O APIC input from the cascaded 8259A as the timer
	 * interrupt input.  So just in case, if only one pin
	 * was found above, try it both directly and through the
	 * 8259A.
	 */
	if (pin1 == -1) {
		pin1 = pin2;
		apic1 = apic2;
		no_pin1 = 1;
	} else if (pin2 == -1) {
		pin2 = pin1;
		apic2 = apic1;
	}

	if (pin1 != -1) {
		/* Ok, does IRQ0 through the IOAPIC work? */
		if (no_pin1) {
			mp_alloc_timer_irq(apic1, pin1);
		} else {
			/*
			 * for edge trigger, it's already unmasked,
			 * so only need to unmask if it is level-trigger
			 * do we really have level trigger timer?
			 */
			int idx;
			idx = find_irq_entry(apic1, pin1, mp_INT);
			if (idx != -1 && irq_trigger(idx))
				unmask_ioapic_irq(irq_get_chip_data(0));
		}

		irq_domain_activate_irq(irq_data);

		if (timer_irq_works())
			goto out;

		local_irq_disable();
		clear_IO_APIC_pin(apic1, pin1);
		if (!no_pin1)
			apic_printk(APIC_QUIET, "..MP-BIOS bug: "
				    "8254 timer not connected to IO-APIC\n");

		apic_printk(APIC_QUIET, KERN_INFO "...trying to set up timer "
			    "(IRQ0) through the 8259A ...\n");
		apic_printk(APIC_QUIET, KERN_INFO
			    "..... (found apic %d pin %d) ...\n", apic2, pin2);
		/*
		 * legacy devices should be connected to IO APIC #0
		 */
		replace_pin_at_irq_node(data, node, apic1, pin1, apic2, pin2);
		irq_domain_activate_irq(irq_data);
		legacy_pic->unmask(0);
		if (timer_irq_works()) {
			apic_printk(APIC_QUIET, KERN_INFO "....... works.\n");
			goto out;
		}
		/*
		 * Cleanup, just in case ...
		 */
		local_irq_disable();
		legacy_pic->mask(0);
		clear_IO_APIC_pin(apic2, pin2);
		apic_printk(APIC_QUIET, KERN_INFO "....... failed.\n");
	}

	panic("IO-APIC + timer doesn't work!");

out:
	local_irq_restore(flags);
}

void __init setup_IO_APIC(void)
{
	int i;

	io_apic_irqs = nr_legacy_irqs() ? ~PIC_IRQS : ~0UL;

	apic_printk(APIC_VERBOSE, "ENABLING IO-APIC IRQs\n");

	/* Allocate IRQ_DOMAIN object for each IO-APIC */
	for_each_ioapic(i) {
		struct ioapic *ip = &ioapics[i];
		struct ioapic_domain_cfg *cfg = &ip->irqdomain_cfg;
		struct mp_ioapic_gsi *gsi_cfg = mp_ioapic_gsi_routing(i);
		int hwirqs = mp_ioapic_pin_count(i);

		if (cfg->type == IOAPIC_DOMAIN_INVALID)
			continue;

		ip->irqdomain = kzalloc(sizeof(struct irq_domain), GFP_KERNEL);
		BUG_ON(!ip->irqdomain);
		ip->irqdomain->ops = cfg->ops;
		ip->irqdomain->host_data = (void *)(unsigned long)i;
		ip->irqdomain->hwirq_max = hwirqs;

		/* Just one single parent */
		ip->irqdomain->parent = &x86_vector_domain;

		if (cfg->type == IOAPIC_DOMAIN_LEGACY ||
		    cfg->type == IOAPIC_DOMAIN_STRICT)
		    	ioapic_dynirq_base = max(ioapic_dynirq_base, gsi_cfg->gsi_end + 1);
	}

	sync_Arb_IDs();

	/* Setup pin <-> IRQ mapping */
	setup_IO_APIC_irqs();

	init_IO_APIC_traps();
	if (nr_legacy_irqs())
		check_timer();

	ioapic_initialized = 1;
}

static struct irq_chip ioapic_chip __read_mostly = {
	.name			= "IO-APIC",
};

static struct irq_chip ioapic_ir_chip __read_mostly = {
	.name			= "IR-IO-APIC",
};

static void mp_irqdomain_get_attr(u32 gsi, struct mp_chip_data *data,
				  struct irq_alloc_info *info)
{
	if (info && info->ioapic_valid) {
		data->trigger = info->ioapic_trigger;
		data->polarity = info->ioapic_polarity;
	} else if (acpi_get_override_irq(gsi, &data->trigger,
					 &data->polarity) < 0) {
		/* PCI interrupts are always active low level triggered. */
		data->trigger = IOAPIC_LEVEL;
		data->polarity = IOAPIC_POL_LOW;
	}
}

static void mp_setup_entry(struct irq_cfg *cfg, struct mp_chip_data *data,
			   struct IO_APIC_route_entry *entry)
{
	memset(entry, 0, sizeof(*entry));
	entry->delivery_mode = apic->irq_delivery_mode;
	entry->dest_mode     = apic->irq_dest_mode;
	entry->dest	     = cfg->dest_apicid;
	entry->vector	     = cfg->vector;
	entry->trigger	     = data->trigger;
	entry->polarity	     = data->polarity;
	/*
	 * Mask level triggered irqs. Edge triggered irqs are masked
	 * by the irq core code in case they fire.
	 */
	if (data->trigger == IOAPIC_LEVEL)
		entry->mask = IOAPIC_MASKED;
	else
		entry->mask = IOAPIC_UNMASKED;
}

static int mp_irqdomain_alloc(struct irq_domain *domain, unsigned int virq,
			      unsigned int nr_irqs, void *arg)
{
	struct irq_data *irq_data;
	struct mp_chip_data *data;
	struct irq_alloc_info *info;
	struct irq_cfg *cfg;
	int ioapic, ret, pin;
	unsigned long flags;

	/* Upper IRQ layer pass it back to us :-; */
	info = (struct irq_alloc_info *)arg;
	if (!info || nr_irqs > 1)
		return -EINVAL;

	irq_data = irq_domain_get_irq_data(domain, virq);
	if (!irq_data)
		return -EINVAL;

	ioapic = mp_irqdomain_ioapic_idx(domain);
	pin = info->ioapic_pin;

	if (irq_find_mapping(domain, (irq_hw_number_t)pin) > 0)
		return -EEXIST;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	info->ioapic_entry = &data->entry;

	/*
	 * !
	 * Call back to parent x86_vector_domain
	 * Call back to local APIC vector alloc
	 *
	 * Local APIC will allocate the vector for,
	 * after this, we have the pin <-> IRQ <-> vector number mapping
	 */
	ret = irq_domain_alloc_irqs_parent(domain, virq, nr_irqs, info);
	if (ret < 0) {
		kfree(data);
		return ret;
	}

	INIT_LIST_HEAD(&data->irq_2_pin);
	irq_data->hwirq = info->ioapic_pin;
	irq_data->chip = (domain->parent == &x86_vector_domain) ?
			  &ioapic_chip : &ioapic_ir_chip;
	irq_data->chip_data = data;

	mp_irqdomain_get_attr(mp_pin_to_gsi(ioapic, pin), data, info);

	cfg = irqd_cfg(irq_data);

	/* Add pin to irq list */
	add_pin_to_irq_node(data, ioapic_alloc_attr_node(info), ioapic, pin);

	local_irq_save(flags);
	if (info->ioapic_entry)
		mp_setup_entry(cfg, data, info->ioapic_entry);
	mp_register_handler(virq, data->trigger);
	if (virq < nr_legacy_irqs())
		legacy_pic->mask(virq);
	local_irq_restore(flags);

	apic_printk(APIC_VERBOSE,
		    "IOAPIC[%d]: Set routing entry (%d-%d -> 0x%x -> IRQ %d Mode:%i Active:%i Dest:%d)\n",
		    ioapic, mpc_ioapic_id(ioapic), pin, cfg->vector,
		    virq, data->trigger, data->polarity, cfg->dest_apicid);

	return 0;
}

void mp_irqdomain_activate(struct irq_domain *domain,
			   struct irq_data *irq_data)
{
	unsigned long flags;
	struct irq_pin_list *entry;
	struct mp_chip_data *data = irq_data->chip_data;

	spin_lock_irqsave(&ioapic_lock, flags);
	for_each_irq_pin(entry, data->irq_2_pin)
		__ioapic_write_entry(entry->apic, entry->pin, data->entry);
	spin_unlock_irqrestore(&ioapic_lock, flags);
}

const struct irq_domain_ops mp_ioapic_irqdomain_ops = {
	.alloc		= mp_irqdomain_alloc,
	.activate	= mp_irqdomain_activate,
};
