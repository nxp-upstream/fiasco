INTERFACE:

#include <types.h>
#include "initcalls.h"
#include <spin_lock.h>
#include "apic.h"
#include "irq_chip_ia32.h"
#include <cxx/bitfield>
#include "irq_mgr.h"
#include "pm.h"

class Acpi_madt;

class Io_apic_entry
{
  friend class Io_apic;

private:
  Unsigned64 _value;

public:
  enum Delivery { Fixed, Lowest_prio, SMI, NMI = 4, INIT, ExtINT = 7 };
  enum Dest_mode { Physical, Logical };
  enum Polarity { High_active, Low_active };
  enum Trigger { Edge, Level };

  Io_apic_entry() {}
  Io_apic_entry(Unsigned8 vector, Delivery d, Dest_mode dm, Polarity p,
                Trigger t, Apic_id dest)
    : _value(  vector_bfm_t::val(vector) | delivery_bfm_t::val(d)
             | mask_bfm_t::val(1)        | dest_mode_bfm_t::val(dm)
             | polarity_bfm_t::val(p)    | trigger_bfm_t::val(t)
             | dest_bfm_t::val(cxx::int_value<Apic_id>(dest) >> 24))
  {}

  CXX_BITFIELD_MEMBER( 0,  7, vector, _value);
  CXX_BITFIELD_MEMBER( 8, 10, delivery, _value);
  CXX_BITFIELD_MEMBER(11, 11, dest_mode, _value);
  CXX_BITFIELD_MEMBER(13, 13, polarity, _value);
  CXX_BITFIELD_MEMBER(15, 15, trigger, _value);
  CXX_BITFIELD_MEMBER(16, 16, mask, _value);
  // support for IRQ remapping
  CXX_BITFIELD_MEMBER(48, 48, format, _value);
  // support for IRQ remapping
  CXX_BITFIELD_MEMBER(49, 63, irt_index, _value);
  CXX_BITFIELD_MEMBER(56, 63, dest, _value);
};

class Io_apic : public Irq_chip_icu, protected Irq_chip_ia32
{
  friend class Jdb_kern_info_io_apic;
  friend class Irq_chip_ia32;

public:
  unsigned nr_pins() const override { return Irq_chip_ia32::nr_pins(); }
  bool reserve(Mword pin) override { return Irq_chip_ia32::reserve(pin); }
  Irq_base *irq(Mword pin) const override { return Irq_chip_ia32::irq(pin); }

private:
  struct Apic
  {
    Unsigned32 volatile adr;
    Unsigned32 dummy[3];
    Unsigned32 volatile data;

    unsigned num_entries();
    Mword read(int reg);
    void modify(int reg, Mword set_bits, Mword del_bits);
    void write(int reg, Mword value);
  } __attribute__((packed));

  Apic *_apic;
  mutable Spin_lock<> _lock;
  unsigned _offset;
  Io_apic *_next;

  static unsigned _nr_pins;
  static Io_apic *_first;
  static Acpi_madt const *_madt;
  static Io_apic_entry *_state_save_area;
};

class Io_apic_mgr : public Irq_mgr, public Pm_object
{
public:
  Io_apic_mgr() { register_pm(Cpu_number::boot_cpu()); }
};

//---------------------------------------------------------------------------
IMPLEMENTATION:

#include "acpi.h"
#include "assert.h"
#include "kmem_mmio.h"
#include "kip.h"
#include "mem.h"
#include "pic.h"
#include "lock_guard.h"
#include "boot_alloc.h"
#include "warn.h"
#include "panic.h"

enum { Print_info = 0 };

Acpi_madt const *Io_apic::_madt;
unsigned Io_apic::_nr_pins;
Io_apic *Io_apic::_first;
Io_apic_entry *Io_apic::_state_save_area;

PUBLIC
Irq_mgr::Chip_pin
Io_apic_mgr::chip_pin(Mword gsi) const override
{
  Io_apic *ioapic = Io_apic::find_apic(gsi);
  if (ioapic)
    return Chip_pin(ioapic, gsi - ioapic->gsi_offset());

  return Chip_pin();
}

PUBLIC
unsigned
Io_apic_mgr::nr_gsis() const override
{
  return Io_apic::nr_gsis();
}

PUBLIC
unsigned
Io_apic_mgr::nr_msis() const override
{ return 0; }

PUBLIC
Mword
Io_apic_mgr::legacy_override(Mword isa_pin) override
{ return Io_apic::legacy_override(isa_pin); }

PUBLIC
void
Io_apic_mgr::pm_on_suspend([[maybe_unused]] Cpu_number cpu) override
{
  assert(cpu == Cpu_number::boot_cpu());
  Io_apic::save_state();
}

PUBLIC
void
Io_apic_mgr::pm_on_resume([[maybe_unused]] Cpu_number cpu) override
{
  assert(cpu == Cpu_number::boot_cpu());
  Pic::disable_all_save();
  Io_apic::restore_state(true);
}


IMPLEMENT inline NEEDS["mem.h"]
Mword
Io_apic::Apic::read(int reg)
{
  adr = reg;
  Mem::barrier();
  return data;
}

IMPLEMENT inline NEEDS["mem.h"]
void
Io_apic::Apic::modify(int reg, Mword set_bits, Mword del_bits)
{
  Mword tmp;
  adr = reg;
  Mem::barrier();
  tmp = data;
  tmp &= ~del_bits;
  tmp |= set_bits;
  data = tmp;
}

IMPLEMENT inline NEEDS["mem.h"]
void
Io_apic::Apic::write(int reg, Mword value)
{
  adr = reg;
  Mem::barrier();
  data = value;
}

IMPLEMENT inline
unsigned
Io_apic::Apic::num_entries()
{
  return (read(1) >> 16) & 0xff;
}

PUBLIC explicit
Io_apic::Io_apic(Unsigned64 phys, unsigned gsi_base)
: Irq_chip_ia32(0), _offset(gsi_base), _next(nullptr)
{
  if constexpr (Print_info)
    printf("IO-APIC: addr=%llx\n", phys);

  auto a = static_cast<Io_apic::Apic *>(Kmem_mmio::map(phys, Config::PAGE_SIZE));
  if (!a)
    panic("Unable to map IO-APIC");

  Kip::k()->add_mem_region(Mem_desc(phys, phys + Config::PAGE_SIZE -1, Mem_desc::Reserved));

  a->write(0, 0);

  _apic = a;
  _pins = a->num_entries() + 1;
  _vec = Boot_alloced::allocate<unsigned char>(_pins);

  if ((_offset + nr_pins()) > _nr_pins)
    _nr_pins = _offset + nr_pins();

  Io_apic **c = &_first;
  while (*c && (*c)->_offset < _offset)
    c = &((*c)->_next);

  _next = *c;
  *c = this;

  Apic_id cpu_phys = ::Apic::apic.cpu(Cpu_number::boot_cpu())->apic_id();

  for (unsigned i = 0; i < _pins; ++i)
    {
      int v = 0x20 + i;
      Io_apic_entry entry(v, Io_apic_entry::Fixed, Io_apic_entry::Physical,
                          Io_apic_entry::High_active, Io_apic_entry::Edge,
                          cpu_phys);
      write_entry(i, entry);
    }
}

PUBLIC inline NEEDS["assert.h", "lock_guard.h"]
Io_apic_entry
Io_apic::read_entry(unsigned i) const
{
  auto guard = lock_guard(_lock);
  Io_apic_entry entry;
  //assert(i <= num_entries());
  entry._value = _apic->read(0x10 + 2 * i)
                 | (Unsigned64{_apic->read(0x11 + 2 * i)} << 32);
  return entry;
}

PUBLIC inline NEEDS["assert.h", "lock_guard.h"]
void
Io_apic::write_entry(unsigned i, Io_apic_entry const &entry)
{
  auto guard = lock_guard(_lock);
  //assert(i <= num_entries());
  _apic->write(0x10 + 2 * i, entry._value);
  _apic->write(0x11 + 2 * i, entry._value >> 32);
}

PROTECTED static FIASCO_INIT
void
Io_apic::read_overrides()
{
  int n = -1;
  for (auto *irq : _madt->iterate<Acpi_madt::Irq_source>())
    {
      ++n;

      if constexpr (Print_info)
        printf("I/O APIC: override[%2u] ISA:%d -> PIN:%u (flags=%x)\n",
               n, irq->src, irq->irq, irq->flags);

      if (irq->irq >= _nr_pins)
        {
          WARN("I/O APIC: override[%u] ISA:%d -> PIN:%u (flags=%x) "
               "points to invalid GSI\n", n, irq->src, irq->irq, irq->flags);
          continue;
        }

      Io_apic *ioapic = find_apic(irq->irq);
      assert(ioapic);
      unsigned pin = irq->irq - ioapic->gsi_offset();
      Irq_chip::Mode mode = ioapic->get_mode(pin);

      unsigned pol = irq->flags & 0x3;
      unsigned trg = (irq->flags >> 2) & 0x3;

      switch (pol)
        {
        default: break;
        case 0: break;
        case 1: mode.polarity() = Mode::Polarity_high; break;
        case 2: break;
        case 3: mode.polarity() = Mode::Polarity_low; break;
        }

      switch (trg)
        {
        default: break;
        case 0: break;
        case 1: mode.level_triggered() = Mode::Trigger_edge; break;
        case 2: break;
        case 3: mode.level_triggered() = Mode::Trigger_level; break;
        }

      ioapic->set_mode(pin, mode);
    }
}

PUBLIC static FIASCO_INIT
Acpi_madt const *
Io_apic::lookup_madt()
{
  if (_madt)
    return _madt;

  _madt = Acpi::find<Acpi_madt const *>("APIC");
  return _madt;
}

PUBLIC static inline
Acpi_madt const *Io_apic::madt() { return _madt; }

PUBLIC static FIASCO_INIT
bool
Io_apic::init_scan_apics()
{
  auto madt = lookup_madt();
  if (madt == nullptr)
    {
      WARN("Could not find APIC in RSDT nor XSDT, skipping initialization\n");
      return false;
    }

  unsigned n_apics = 0;
  for (auto *ioapic : madt->iterate<Acpi_madt::Io_apic>())
    {
      Io_apic *apic = new Boot_object<Io_apic>(ioapic->addr, ioapic->irq_base);

      if constexpr (Print_info)
        {
          printf("I/O APIC[%2d]: %u pins\n", n_apics, apic->nr_pins());
          apic->dump();
        }

      ++n_apics;
    }

  if (!n_apics)
    {
      WARN("I/O APIC: Could not find I/O APIC in MADT, skipping initialization\n");
      return false;
    }

  if constexpr (Print_info)
    printf("I/O APIC: Dual 8259 %s\n",
           madt->apic_flags & 1 ? "present" : "absent");

  return true;
}

PUBLIC static FIASCO_INIT
void
Io_apic::init(Cpu_number)
{
  if (!Irq_mgr::mgr)
    Irq_mgr::mgr = new Boot_object<Io_apic_mgr>();

  _state_save_area = new Boot_object<Io_apic_entry>[_nr_pins];
  read_overrides();

  // in the case we use the IO-APIC not the PIC we can dynamically use
  // INT vectors from 0x20 to 0x2f too
  _vectors.add_free(0x20, 0x30);
};

PUBLIC static
void
Io_apic::save_state()
{
  for (Io_apic *apic = _first; apic; apic = apic->_next)
    for (unsigned pin = 0; pin < apic->_pins; ++pin)
      _state_save_area[apic->_offset + pin] = apic->read_entry(pin);
}

PUBLIC static
void
Io_apic::restore_state(bool set_boot_cpu = false)
{
  Apic_id cpu_phys{0};
  if (set_boot_cpu)
    cpu_phys = ::Apic::apic.cpu(Cpu_number::boot_cpu())->apic_id();

  for (Io_apic *a = _first; a; a = a->_next)
    for (unsigned i = 0; i < a->_pins; ++i)
      {
        Io_apic_entry e = _state_save_area[a->_offset + i];
        if (set_boot_cpu && e.format() == 0)
          e.dest() = cxx::int_value<Apic_id>(cpu_phys);
        a->write_entry(i, e);
      }
}

PUBLIC static
unsigned
Io_apic::nr_gsis()
{ return _nr_pins; }

PUBLIC static
Mword
Io_apic::legacy_override(Mword isa_pin)
{
  if (!_madt)
    return isa_pin;

  for (auto *irq : _madt->iterate<Acpi_madt::Irq_source>())
    if (irq->src == isa_pin)
      return irq->irq;

  return isa_pin;
}

PUBLIC
void
Io_apic::dump()
{
  for (unsigned pin = 0; pin < _nr_pins; ++pin)
    {
      Io_apic_entry e = read_entry(pin);
      printf("  PIN[%2u%c]: vector=%u, del=%u, dm=%s, dest=%u (%s, %s)\n",
             pin, e.mask() ? 'm' : '.', e.vector().get(), e.delivery().get(),
             e.dest_mode() ? "logical" : "physical", e.dest().get(),
             e.polarity() ? "low" : "high", e.trigger() ? "level" : "edge");
    }
}

PUBLIC inline
bool
Io_apic::valid() const { return _apic; }

PRIVATE inline NEEDS["assert.h", "lock_guard.h"]
void
Io_apic::_mask(Mword pin)
{
  auto guard = lock_guard(_lock);
  //assert(pin <= _apic->num_entries());
  _apic->modify(0x10 + pin * 2, 1UL << 16, 0);
}

PRIVATE inline NEEDS["assert.h", "lock_guard.h"]
void
Io_apic::_unmask(Mword pin)
{
  auto guard = lock_guard(_lock);
  //assert(pin <= _apic->num_entries());
  _apic->modify(0x10 + pin * 2, 0, 1UL << 16);
}

PUBLIC inline NEEDS["assert.h", "lock_guard.h"]
bool
Io_apic::masked(Mword pin)
{
  auto guard = lock_guard(_lock);
  //assert(pin <= _apic->num_entries());
  return _apic->read(0x10 + pin * 2) & (1UL << 16);
}

PUBLIC inline
void
Io_apic::sync()
{
  _apic->data; // read volatile data
}

PUBLIC inline NEEDS["assert.h", "lock_guard.h"]
void
Io_apic::set_dest(unsigned irq, Apic_id dst)
{
  auto guard = lock_guard(_lock);
  //assert(irq <= _apic->num_entries());
  _apic->modify(0x11 + irq * 2,
                cxx::int_value<Apic_id>(dst) & (~0UL << 24), ~0UL << 24);
}

PUBLIC inline
unsigned
Io_apic::gsi_offset() const { return _offset; }

PUBLIC static
Io_apic *
Io_apic::find_apic(unsigned gsi)
{
  for (Io_apic *a = _first; a; a = a->_next)
    {
      if (a->_offset <= gsi && a->_offset + a->_pins > gsi)
        return a;
    }

  return nullptr;
};

PUBLIC
void
Io_apic::mask(Mword pin) override
{
  _mask(pin);
  sync();
}

PUBLIC
void
Io_apic::ack(Mword) override
{
  ::Apic::irq_ack();
}

PUBLIC
void
Io_apic::mask_and_ack(Mword pin) override
{
  _mask(pin);
  sync();
  ::Apic::irq_ack();
}

PUBLIC
void
Io_apic::unmask(Mword pin) override
{
  _unmask(pin);
}

PUBLIC
bool
Io_apic::set_cpu(Mword pin, Cpu_number cpu) override
{
  set_dest(pin, ::Apic::apic.cpu(cpu)->apic_id());
  return true;
}

PROTECTED static inline
Mword
Io_apic::to_io_apic_trigger(Irq_chip::Mode mode)
{
  return mode.level_triggered()
         ? Io_apic_entry::Level
         : Io_apic_entry::Edge;
}

PROTECTED static inline
Mword
Io_apic::to_io_apic_polarity(Irq_chip::Mode mode)
{
  return mode.polarity() == Irq_chip::Mode::Polarity_high
         ? Io_apic_entry::High_active
         : Io_apic_entry::Low_active;
}

PUBLIC int
Io_apic::set_mode(Mword pin, Mode mode) override
{
  if (!mode.set_mode())
    return 0;

  Io_apic_entry entry = read_entry(pin);
  entry.polarity() = to_io_apic_polarity(mode);
  entry.trigger() = to_io_apic_trigger(mode);
  write_entry(pin, entry);

  return 0;
}

PUBLIC inline
Irq_chip::Mode
Io_apic::get_mode(Mword pin)
{
  Io_apic_entry entry = read_entry(pin);
  Mode mode(Mode::Set_irq_mode);

  mode.polarity() = entry.polarity() == Io_apic_entry::High_active
                    ? Mode::Polarity_high
                    : Mode::Polarity_low;

  mode.level_triggered() = entry.trigger() == Io_apic_entry::Level
                           ? Mode::Trigger_level
                           : Mode::Trigger_edge;

  return mode;
}

PUBLIC
bool
Io_apic::is_edge_triggered(Mword pin) const override
{
  Io_apic_entry entry = read_entry(pin);
  return entry.trigger() == Io_apic_entry::Edge;
}

PUBLIC
bool
Io_apic::attach(Irq_base *irq, Mword pin, bool init = true) override
{
  unsigned v = valloc<Io_apic>(irq, pin, 0, init);

  if (!v)
    return false;

  Io_apic_entry entry = read_entry(pin);
  entry.vector() = v;
  write_entry(pin, entry);

  return true;
}

PUBLIC
void
Io_apic::detach(Irq_base *irq) override
{
  extern char entry_int_apic_ignore[];
  Mword n = irq->pin();
  mask(n);
  vfree(irq, &entry_int_apic_ignore);
  Irq_chip_icu::detach(irq);
}

PUBLIC static inline
bool
Io_apic::active()
{ return _first; }

//---------------------------------------------------------------------------
IMPLEMENTATION [debug]:

PUBLIC inline
char const *
Io_apic::chip_type() const override
{ return "IO-APIC"; }
