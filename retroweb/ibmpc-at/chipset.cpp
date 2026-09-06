#include "chipset.h"

#include <cstring>

namespace ibmpcat {

namespace {
// Standard AT DMA page-register port assignments (separate 74-series glue
// logic, not part of the 8237 itself). 0x8F is conventionally wired to
// DMA1 channel 0 / the refresh cascade rather than anything DMA2's channel
// 4 (the inter-chip cascade line) meaningfully uses -- nothing in this
// emulator depends on that value, so the exact real-hardware rationale for
// 0x8F isn't load-bearing here.
bool page_port_map(uint16_t port, int &controller, int &channel) {
    switch (port) {
        case 0x87: controller = 1; channel = 0; return true;
        case 0x83: controller = 1; channel = 1; return true;
        case 0x81: controller = 1; channel = 2; return true;
        case 0x82: controller = 1; channel = 3; return true;
        case 0x8F: controller = 2; channel = 0; return true;
        case 0x8B: controller = 2; channel = 1; return true;
        case 0x89: controller = 2; channel = 2; return true;
        case 0x8A: controller = 2; channel = 3; return true;
        default: return false;
    }
}
}  // namespace

void Chipset::reset() {
    port61_ = 0;
    refresh_toggle_ = false;
    fdc_irq_prev_ = false;
    kbc_irq_prev_ = false;
    hdd_irq_prev_ = false;
    pic_master.reset();
    pic_slave.reset();
    pit.reset();
    kbc.reset();
    dma1.reset();
    dma2.reset();
    fdc.reset();
    ega.reset();
    hdd.reset();
    // mem/rom_ contents deliberately survive reset() (a CPU/warm reset
    // doesn't erase RAM or reflash the BIOS on real hardware, and doesn't
    // un-write-protect ROM either); only load_rom() and the constructor's
    // zero-init touch them. `cmos` likewise deliberately survives reset()
    // here -- it's genuine battery-backed non-volatile RAM on real
    // hardware, unaffected by anything short of the battery itself dying
    // (not modeled). This is exactly the bug that bit a diagnostic harness
    // during Phase 3: calling reset() after seeding the factory CMOS
    // configuration used to silently wipe it back to zero. See
    // IBM_PCAT_REVIEW.md §8.
}

void Chipset::load_rom(uint32_t addr, const uint8_t *data, std::size_t len) {
    std::memcpy(mem.data() + addr, data, len);
    for (std::size_t i = 0; i < len; ++i) rom_[addr + i] = true;
}

uint8_t Chipset::mem_read(uint32_t addr) {
    if (!kbc.a20_enabled()) addr &= 0xFFFFF;  // gate closed: real 20-bit wraparound
    if (ega.owns_mem(addr)) return ega.mem_read(addr);
    if (addr >= mem.size()) return 0xFF;      // nothing populated up there on this system
    return mem[addr];
}
void Chipset::mem_write(uint32_t addr, uint8_t v) {
    if (!kbc.a20_enabled()) addr &= 0xFFFFF;
    if (ega.owns_mem(addr)) { ega.mem_write(addr, v); return; }
    if (addr >= mem.size()) return;
    if (rom_[addr]) return;  // ROM: writes ignored, matching real hardware
    mem[addr] = v;
}

uint8_t Chipset::port61() const {
    uint8_t v = uint8_t(port61_ & 0x03);
    if (pit.channel2_output()) v = uint8_t(v | 0x20);
    if (refresh_toggle_) v = uint8_t(v | 0x10);
    return v;
}
void Chipset::set_port61(uint8_t v) {
    port61_ = uint8_t(v & 0x03);
    pit.set_gate2((v & 0x01) != 0);
}

uint8_t Chipset::io_in(uint16_t port) {
    if (pic_master.owns(port)) return pic_master.in(port);
    if (pic_slave.owns(port)) return pic_slave.in(port);
    if (pit.owns(port)) return pit.in(port);
    if (kbc.owns(port)) return kbc.in(port);
    if (cmos.owns(port)) return cmos.in(port);
    if (dma1.owns(port)) return dma1.in(port);
    if (dma2.owns(port)) return dma2.in(port);
    if (fdc.owns(port)) return fdc.in(port);
    if (ega.owns_port(port)) return ega.in(port);
    if (hdd.owns(port)) return hdd.in(port);
    if (port == 0x61) return port61();
    int controller, channel;
    if (page_port_map(port, controller, channel)) return (controller == 1 ? dma1 : dma2).page(channel);
    return 0xFF;
}
void Chipset::io_out(uint16_t port, uint8_t v) {
    if (pic_master.owns(port)) { pic_master.out(port, v); return; }
    if (pic_slave.owns(port)) { pic_slave.out(port, v); return; }
    if (pit.owns(port)) { pit.out(port, v); return; }
    if (kbc.owns(port)) { kbc.out(port, v); return; }
    if (cmos.owns(port)) { cmos.out(port, v); return; }
    if (dma1.owns(port)) { dma1.out(port, v); return; }
    if (dma2.owns(port)) { dma2.out(port, v); return; }
    if (fdc.owns(port)) { fdc.out(port, v); return; }
    if (ega.owns_port(port)) { ega.out(port, v); return; }
    if (hdd.owns(port)) { hdd.out(port, v); return; }
    if (port == 0x61) { set_port61(v); return; }
    if (port == 0x80) { last_post_code_ = v; return; }
    if (port == 0xE9) { debug_console_.push_back(char(v)); return; }
    int controller, channel;
    if (page_port_map(port, controller, channel)) { (controller == 1 ? dma1 : dma2).set_page(channel, v); return; }
    // unmapped port -- real hardware: write vanishes (open bus)
}

uint16_t Chipset::io_in16(uint16_t port) {
    // The hard disk data register is inherently 16-bit at a single port
    // address -- 0x1F1 is a completely different register (Error), not
    // "the high byte of 0x1F0". Every other port composes from two 8-bit
    // accesses, which is what real software actually does for them.
    if (port == 0x1F0) return hdd.data_in16();
    return uint16_t(io_in(port)) | (uint16_t(io_in(uint16_t(port + 1))) << 8);
}
void Chipset::io_out16(uint16_t port, uint16_t v) {
    if (port == 0x1F0) { hdd.data_out16(v); return; }
    io_out(port, uint8_t(v & 0xFF));
    io_out(uint16_t(port + 1), uint8_t(v >> 8));
}

cpu80286::Bus Chipset::make_bus() {
    cpu80286::Bus bus;
    bus.read = [this](uint32_t addr) { return mem_read(addr); };
    bus.write = [this](uint32_t addr, uint8_t v) { mem_write(addr, v); };
    bus.in = [this](uint16_t port) { return io_in(port); };
    bus.out = [this](uint16_t port, uint8_t v) { io_out(port, v); };
    bus.in16 = [this](uint16_t port) { return io_in16(port); };
    bus.out16 = [this](uint16_t port, uint16_t v) { io_out16(port, v); };
    return bus;
}

void Chipset::tick(uint64_t cpu_cycles, double cpu_hz) {
    int ch0_rises = pit.tick(cpu_cycles, cpu_hz);
    for (int i = 0; i < ch0_rises; ++i) pic_master.raise(0);
    refresh_toggle_ = !refresh_toggle_;

    fdc.tick(cpu_cycles);
    ega.tick(cpu_cycles);
    // The FDC/DMA handoff: once a paced READ/WRITE DATA transfer's real-time
    // wait has elapsed, perform the whole block copy in one step (see
    // fdc765.h's file header for why this is a bulk copy rather than a
    // byte-by-byte DMA dance) using whichever of DMA1's address/count/page
    // registers were programmed for channel 2 (the floppy's fixed DMA
    // channel on a genuine AT). DMA addresses bypass the A20 gate --  a
    // real AT's A20 gate sits specifically in the CPU's own address path,
    // not the DMA controller's -- so this goes straight at `mem`, not
    // through mem_read/mem_write.
    if (fdc.transfer_ready() && !dma1.channel_masked(2)) {
        uint16_t dma_len16 = uint16_t(dma1.count(2) + 1);  // 8237 count register is programmed as N-1
        std::size_t len = std::min(fdc.transfer_length(), std::size_t(dma_len16));
        uint32_t addr = (uint32_t(dma1.page(2)) << 16) | dma1.address(2);
        uint8_t *img = fdc.transfer_image_ptr();
        if (img != nullptr) {
            if (fdc.transfer_is_write()) {
                for (std::size_t i = 0; i < len; ++i) img[i] = mem[(addr + i) & 0xFFFFF];
            } else {
                for (std::size_t i = 0; i < len; ++i) mem[(addr + i) & 0xFFFFF] = img[i];
            }
        }
        for (std::size_t i = 0; i < len; ++i) dma1.advance(2);
        fdc.finish_transfer(len);
    }
    // Edge-triggered: raise IRQ6 only on the 0->1 transition, matching
    // genuine ISA wiring -- see the fdc_irq_prev_ comment in chipset.h.
    bool fdc_irq_now = fdc.irq_pending();
    if (fdc_irq_now && !fdc_irq_prev_) pic_master.raise(6);
    fdc_irq_prev_ = fdc_irq_now;

    // IRQ1 (keyboard) was never wired to the PIC at all until this was
    // found via a real end-to-end boot test: a keypress sat in i8042's
    // output buffer forever, since nothing ever told the PIC about it.
    // Same edge-triggering discipline as IRQ6. See IBM_PCAT_REVIEW.md §9.
    bool kbc_irq_now = kbc.irq1_pending();
    if (kbc_irq_now && !kbc_irq_prev_) pic_master.raise(1);
    kbc_irq_prev_ = kbc_irq_now;

    // IRQ14 (hard disk), on the slave PIC (global IRQ14 = slave line 6).
    hdd.tick(cpu_cycles);
    bool hdd_irq_now = hdd.irq_pending();
    if (hdd_irq_now && !hdd_irq_prev_) pic_slave.raise(6);
    hdd_irq_prev_ = hdd_irq_now;
}

int Chipset::poll_interrupt() {
    if (pic_slave.has_interrupt()) pic_master.raise(2);
    else pic_master.lower(2);
    if (!pic_master.has_interrupt()) return -1;
    if (pic_master.peek_highest_pending() == 2) {
        pic_master.acknowledge();  // completes the master's own INTA side effects; vector discarded
        return pic_slave.acknowledge();
    }
    return pic_master.acknowledge();
}

}  // namespace ibmpcat
