/*
 * \brief  Audio driver BSD API emulation
 * \author Josef Soentgen
 * \date   2014-11-16
 */

/*
 * Copyright (C) 2014-2015 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

/* Genode includes */
#include <base/allocator_avl.h>
#include <base/object_pool.h>
#include <dataspace/client.h>
#include <io_port_session/connection.h>
#include <io_mem_session/connection.h>
#include <pci_session/connection.h>
#include <pci_device/client.h>

/* local includes */
#include "bsd.h"
#include <extern_c_begin.h>
# include <bsd_emul.h>
# include <dev/pci/pcidevs.h>
#include <extern_c_end.h>


extern "C" int probe_cfdata(struct pci_attach_args *);

namespace {

class Pci_driver : public Bsd::Bus_driver
{
	public:

		enum Pci_config { IRQ = 0x3c, CMD = 0x4,
		                  CMD_IO = 0x1, CMD_MEMORY = 0x2, CMD_MASTER = 0x4 };

	private:

		struct pci_attach_args _pa { 0, 0, 0, 0, 0 };

		Pci::Connection        _pci;
		Pci::Device_capability _cap;

		Genode::Io_port_connection *_io_port { nullptr };

		/**
		 * The Dma_region_manager provides memory used for DMA
		 * and manages its mappings.
		 */
		struct Dma_region_manager : public Genode::Allocator_avl
		{
			enum { BACKING_STORE_SIZE = 1024 * 1024 };

			Genode::addr_t base;
			Genode::addr_t mapped_base;

			bool _dma_initialized { false };

			Pci_driver &_drv;

			Dma_region_manager(Genode::Allocator &alloc, Pci_driver &drv)
			: Genode::Allocator_avl(&alloc), _drv(drv) { }

			Genode::addr_t alloc(Genode::size_t size, int align)
			{
				using namespace Genode;

				if (!_dma_initialized) {
					try {
						Ram_dataspace_capability cap = _drv._alloc_dma_memory(BACKING_STORE_SIZE);
						mapped_base = (addr_t)env()->rm_session()->attach(cap);
						base        = Dataspace_client(cap).phys_addr();

						Allocator_avl::add_range(mapped_base, BACKING_STORE_SIZE);
					} catch (...) {
						PERR("alloc DMA memory failed");
						return 0;
					}
					_dma_initialized = true;
				}

				void *ptr = nullptr;
				bool  err = Allocator_avl::alloc_aligned(size, &ptr, align).is_error();

				return err ? 0 : (addr_t)ptr;
			}

			void free(Genode::addr_t virt, Genode::size_t size) {
				Genode::Allocator_avl::free((void*)virt, size); }

			Genode::addr_t virt_to_phys(Genode::addr_t virt) {
				return virt - mapped_base + base; }

			Genode::addr_t phys_to_virt(Genode::addr_t phys) {
				return phys - base + mapped_base; }

		} _dma_region_manager;

		/**
		 * Scan pci bus for sound devices
		 */
		Pci::Device_capability _scan_pci(Pci::Device_capability const &prev)
		{
			Pci::Device_capability cap;
			/* shift values for Pci interface used by Genode */
			cap = _pci.next_device(prev, PCI_CLASS_MULTIMEDIA << 16,
			                             PCI_CLASS_MASK << 16);
			if (prev.valid())
				_pci.release_device(prev);
			return cap;
		}

		/**
		 * Allocate DMA memory from the PCI driver
		 */
		Genode::Ram_dataspace_capability _alloc_dma_memory(Genode::size_t size)
		{
			try {
				/* trigger that the device gets assigned to this driver (needed by IOMMUs) */
				for (unsigned i = 0; i < 2; i++)
					try {
						_pci.config_extended(_cap);
						break;
					} catch (Pci::Device::Quota_exceeded) {
						Genode::env()->parent()->upgrade(_pci.cap(), "ram_quota=4096");
					}

				char buf[32];
				Genode::snprintf(buf, sizeof(buf), "ram_quota=%zu", size);
				Genode::env()->parent()->upgrade(_pci.cap(), buf);

				return _pci.alloc_dma_buffer(size);
			} catch (...) { return Genode::Ram_dataspace_capability(); }
		}

	public:

		Pci_driver() : _dma_region_manager(*Genode::env()->heap(), *this) { }

		Pci::Device_capability cap() { return _cap; }

		Pci::Connection &pci() { return _pci; }

		int probe()
		{
			/*
			 * We hide ourself in the bus_dma_tag_t as well as
			 * in the pci_chipset_tag_t field because they are
			 * used in all pci or bus related functions and are
			 * our access window, hence.
			 */
			_pa.pa_dmat = (bus_dma_tag_t)this;
			_pa.pa_pc   = (pci_chipset_tag_t)this;

			int found = 0;
			while ((_cap = _scan_pci(_cap)).valid()) {
				Pci::Device_client device(_cap);

				uint8_t bus, dev, func;
				device.bus_address(&bus, &dev, &func);

				/* XXX until we get the platform_drv, we blacklist HDMI/DP HDA devices */
				if (device.device_id() == PCI_PRODUCT_INTEL_CORE4G_HDA_2) {
					PWRN("ignore %u:%u:%u device, Intel Core 4G HDA not supported",
					     bus, dev, func);
					continue;
				}

				/* we do the shifting to match OpenBSD's assumptions */
				_pa.pa_tag   = 0x80000000UL | (bus << 16) | (dev << 11) | (func << 8);
				_pa.pa_class = device.class_code() << 8;
				_pa.pa_id    = device.vendor_id() | device.device_id() << 16;

				if (probe_cfdata(&_pa)) {
					found++;
					break;
				}
			}

			return found;
		}

		/**************************
		 ** Bus_driver interface **
		 **************************/

		Genode::Irq_session_capability irq_session() override {
			return Pci::Device_client(_cap).irq(0); }

		Genode::addr_t alloc(Genode::size_t size, int align) override {
			return _dma_region_manager.alloc(size, align); }

		void free(Genode::addr_t virt, Genode::size_t size) override {
			_dma_region_manager.free(virt, size); }

		Genode::addr_t virt_to_phys(Genode::addr_t virt) override {
			return _dma_region_manager.virt_to_phys(virt); }

		Genode::addr_t phys_to_virt(Genode::addr_t phys) override {
			return _dma_region_manager.phys_to_virt(phys); }
};


/**********************
 ** Bus space helper **
 **********************/

struct Bus_space
{
	virtual unsigned read_1(unsigned long address) = 0;
	virtual unsigned read_2(unsigned long address) = 0;
	virtual unsigned read_4(unsigned long address) = 0;

	virtual void write_1(unsigned long address, unsigned char  value) = 0;
	virtual void write_2(unsigned long address, unsigned short value) = 0;
	virtual void write_4(unsigned long address, unsigned int   value) = 0;
};


/*********************
 ** I/O port helper **
 *********************/

struct Io_port : public Bus_space
{
	Genode::Io_port_session_client _io;
	Genode::addr_t                 _base;

	Io_port(Genode::addr_t base, Genode::Io_port_session_capability cap)
	: _io(cap), _base(base) { }

	unsigned read_1(unsigned long address) {
		return _io.inb(_base + address); }

	unsigned read_2(unsigned long address) {
		return _io.inw(_base + address); }

	unsigned read_4(unsigned long address) {
		return _io.inl(_base + address); }

	void write_1(unsigned long address, unsigned char value) {
		_io.outb(_base + address, value); }

	void write_2(unsigned long address, unsigned short value) {
		_io.outw(_base + address, value); }

	void write_4(unsigned long address, unsigned int value) {
		_io.outl(_base + address, value); }
};


/***********************
 ** I/O memory helper **
 ***********************/

struct Io_memory : public Bus_space
{
	Genode::Io_mem_session_client       _mem;
	Genode::Io_mem_dataspace_capability _mem_ds;
	Genode::addr_t                      _vaddr;

	Io_memory(Genode::addr_t base, Genode::Io_mem_session_capability cap)
	:
		_mem(cap),
		_mem_ds(_mem.dataspace())
	{
		if (!_mem_ds.valid())
			throw Genode::Exception();

		_vaddr = Genode::env()->rm_session()->attach(_mem_ds);
		_vaddr |= base & 0xfff;
	}

	unsigned read_1(unsigned long address) {
		return *(volatile unsigned char*)(_vaddr + address); }

	unsigned read_2(unsigned long address) {
		return *(volatile unsigned short*)(_vaddr + address); }

	unsigned read_4(unsigned long address) {
		return *(volatile unsigned int*)(_vaddr + address); }

	void write_1(unsigned long address, unsigned char value) {
		*(volatile unsigned char*)(_vaddr + address) = value; }

	void write_2(unsigned long address, unsigned short value) {
		*(volatile unsigned short*)(_vaddr + address) = value; }

	void write_4(unsigned long address, unsigned int value) {
		*(volatile unsigned int*)(_vaddr + address) = value; }
};

} /* anonymous namespace */


int Bsd::probe_drivers()
{
	PINF("--- probe drivers ---");
	static Pci_driver drv;
	return drv.probe();
}


/**********************
 ** dev/pci/pcivar.h **
 **********************/

extern "C" int pci_matchbyid(struct pci_attach_args *pa, const struct pci_matchid *ids, int num)
{
	pci_vendor_id_t  vid = PCI_VENDOR(pa->pa_id);
	pci_product_id_t pid = PCI_PRODUCT(pa->pa_id);

	for (int i = 0; i < num; i++) {
		if (vid == ids[i].pm_vid && pid == ids[i].pm_pid)
			return 1;
	}

	return 0;
}


extern "C" int pci_mapreg_map(struct pci_attach_args *pa,
                              int reg, pcireg_t type,
                              int flags, bus_space_tag_t *tagp,
                              bus_space_handle_t *handlep, bus_addr_t *basep,
                              bus_size_t *sizep, bus_size_t maxsize)
{
	/* calculate BAR from given register */
	int r = (reg - 0x10) / 4;

	Pci_driver *drv = (Pci_driver*)pa->pa_pc;

	Pci::Device_capability cap = drv->cap();
	Pci::Device_client device(cap);
	Pci::Device::Resource res = device.resource(r);

	switch (res.type()) {
	case Pci::Device::Resource::IO:
		{
			Io_port *iop = new (Genode::env()->heap())
			                   Io_port(res.base(), device.io_port(r));
			*tagp = (Genode::addr_t) iop;
			break;
		}
	case Pci::Device::Resource::MEMORY:
		{
			Io_memory *iom = new (Genode::env()->heap())
			                     Io_memory(res.base(), device.io_mem(r));
			*tagp = (Genode::addr_t) iom;
			break;
		}
	case Pci::Device::Resource::INVALID:
		{
			PERR("PCI resource type invalid");
			return -1;
		}
	}

	*handlep = res.base();

	if (basep != 0)
		*basep = res.base();
	if (sizep != 0)
		*sizep = maxsize > 0 && res.size() > maxsize ? maxsize : res.size();

	/* enable bus master and I/O or memory bits */
	uint16_t cmd = device.config_read(Pci_driver::CMD, Pci::Device::ACCESS_16BIT);
	if (res.type() == Pci::Device::Resource::IO) {
		cmd &= ~Pci_driver:: CMD_MEMORY;
		cmd |= Pci_driver::CMD_IO;
	} else {
		cmd &= ~Pci_driver::CMD_IO;
		cmd |= Pci_driver::CMD_MEMORY;
	}

	cmd |= Pci_driver::CMD_MASTER;
	device.config_write(Pci_driver::CMD, cmd, Pci::Device::ACCESS_16BIT);

	return 0;
}


/***************************
 ** machine/pci_machdep.h **
 ***************************/

extern "C" pcireg_t pci_conf_read(pci_chipset_tag_t pc, pcitag_t tag, int reg)
{
	Pci_driver *drv = (Pci_driver *)pc;
	Pci::Device_client device(drv->cap());
	return device.config_read(reg, Pci::Device::ACCESS_32BIT);
}


extern "C" void pci_conf_write(pci_chipset_tag_t pc, pcitag_t tag, int reg,
                               pcireg_t val)
{
	Pci_driver *drv = (Pci_driver *)pc;
	Pci::Device_client device(drv->cap());
	return device.config_write(reg, val, Pci::Device::ACCESS_32BIT);
}


/*******************
 ** machine/bus.h **
 *******************/

extern "C" u_int8_t bus_space_read_1(bus_space_tag_t space,
                                     bus_space_handle_t handle,
                                     bus_size_t offset)
{
	Bus_space *bus = (Bus_space*)space;
	return bus->read_1(offset);
}


extern "C" u_int16_t bus_space_read_2(bus_space_tag_t space,
                                      bus_space_handle_t handle,
                                      bus_size_t offset)
{
	Bus_space *bus = (Bus_space*)space;
	return bus->read_2(offset);
}


extern "C" u_int32_t bus_space_read_4(bus_space_tag_t space,
                                      bus_space_handle_t handle,
                                      bus_size_t offset)
{
	Bus_space *bus = (Bus_space*)space;
	return bus->read_4(offset);
}


extern "C" void bus_space_write_1(bus_space_tag_t space,
                                  bus_space_handle_t handle,
                                  bus_size_t offset, u_int8_t value)
{
	Bus_space *bus = (Bus_space*)space;
	bus->write_1(offset, value);
}


extern "C" void bus_space_write_2(bus_space_tag_t space,
                                  bus_space_handle_t handle,
                                  bus_size_t offset, u_int16_t value)
{
	Bus_space *bus = (Bus_space*)space;
	bus->write_2(offset, value);
}


extern "C" void bus_space_write_4(bus_space_tag_t space,
                                  bus_space_handle_t handle,
                                  bus_size_t offset, u_int32_t value)
{
	Bus_space *bus = (Bus_space*)space;
	bus->write_4(offset, value);
}


extern "C" int bus_dmamap_create(bus_dma_tag_t tag, bus_size_t size, int nsegments,
                                 bus_size_t maxsegsz, bus_size_t boundart,
                                 int flags, bus_dmamap_t *dmamp)
{
	struct bus_dmamap *map;
	map = (struct bus_dmamap*) malloc(sizeof(struct bus_dmamap), M_DEVBUF, M_ZERO);

	map->size      = size;
	map->maxsegsz  = maxsegsz;
	map->nsegments = nsegments;

	*dmamp = map;

	return 0;
}


extern "C" void bus_dmamap_destroy(bus_dma_tag_t tag, bus_dmamap_t map) {
	free(map, 0, 0); }


extern "C" int bus_dmamap_load(bus_dma_tag_t tag, bus_dmamap_t dmam, void *buf,
                        bus_size_t buflen, struct proc *p, int flags)
{
	Bsd::Bus_driver *drv = (Bsd::Bus_driver *)tag;

	Genode::addr_t virt      = (Genode::addr_t)buf;
	dmam->dm_segs[0].ds_addr = drv->virt_to_phys(virt);

	return 0;
}


extern "C" void bus_dmamap_unload(bus_dma_tag_t, bus_dmamap_t)
{
	PDBG("not implemented, called from %p", __builtin_return_address(0));
}


extern "C" int bus_dmamem_alloc(bus_dma_tag_t tag, bus_size_t size, bus_size_t alignment,
                                bus_size_t boundary, bus_dma_segment_t *segs, int nsegs,
                                int *rsegs, int flags)
{
	Bsd::Bus_driver *drv = (Bsd::Bus_driver *)tag;

	Genode::addr_t virt = drv->alloc(size, Genode::log2(alignment));
	if (virt == 0)
		return -1;

	segs->ds_addr = drv->virt_to_phys(virt);
	segs->ds_size = size;
	*rsegs        = 1;

	return 0;
}


extern "C" void bus_dmamem_free(bus_dma_tag_t tag, bus_dma_segment_t *segs, int nsegs)
{
	Bsd::Bus_driver *drv = (Bsd::Bus_driver *)tag;

	for (int i = 0; i < nsegs; i++) {
		Genode::addr_t phys = (Genode::addr_t)segs[i].ds_addr;
		Genode::addr_t virt = drv->phys_to_virt(phys);

		drv->free(virt, segs[i].ds_size);
	}
}


extern "C" int bus_dmamem_map(bus_dma_tag_t tag, bus_dma_segment_t *segs, int nsegs,
                              size_t size, caddr_t *kvap, int flags)
{
	if (nsegs > 1) {
		PERR("%s: cannot map more than 1 segment", __func__);
		return -1;
	}

	Bsd::Bus_driver *drv = (Bsd::Bus_driver *)tag;

	Genode::addr_t phys = (Genode::addr_t)segs[0].ds_addr;
	Genode::addr_t virt = drv->phys_to_virt(phys);

	*kvap = (caddr_t)virt;

	return 0;
}


extern "C" void bus_dmamem_unmap(bus_dma_tag_t, caddr_t, size_t) { }


extern "C" paddr_t bus_dmamem_mmap(bus_dma_tag_t, bus_dma_segment_t *,
                                   int, off_t, int, int)
{
	PDBG("not implemented, called from %p", __builtin_return_address(0));
	return 0;
}
