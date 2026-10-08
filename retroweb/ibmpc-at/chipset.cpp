#include "chipset.h"

#include <cstring>

namespace ibmpcat {

namespace {
// AT DMA page-register ports (74-series glue, not part of the 8237).
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
    held_clocks_ = 0;
    fdc_irq_prev_ = false;
    kbc_irq_prev_ = false;
    hdd_irq_prev_ = false;
    ega_irq_prev_ = false;
    pic_master.reset();
    pic_slave.reset();
    pit.reset();
    kbc.reset();
    dma1.reset();
    dma2.reset();
    fdc.reset();
    ega.reset();
    hdd.reset();
    speaker.reset();
    // mem, rom_ and cmos survive reset: RAM and CMOS are not cleared by a warm reset.
}

void Chipset::load_rom(uint32_t addr, const uint8_t *data, std::size_t len) {
    std::memcpy(mem.data() + addr, data, len);
    for (std::size_t i = 0; i < len; ++i) rom_[addr + i] = true;
}

uint8_t Chipset::mem_read(uint32_t addr) {
    if (!kbc.a20_enabled()) addr &= ~0x100000u;  // the gate holds A20 low, nothing else
    if (ega.owns_mem(addr)) return ega.mem_read(addr);
    if (addr >= kRomAlias) return is_rom(addr) ? mem[addr - kRomAliasOffset] : 0xFF;
    if (addr >= mem.size()) return 0xFF;      // unpopulated
    return mem[addr];
}
void Chipset::mem_write(uint32_t addr, uint8_t v) {
    if (!kbc.a20_enabled()) addr &= ~0x100000u;
    if (ega.owns_mem(addr)) { ega.mem_write(addr, v); return; }
    if (addr >= mem.size()) return;
    if (rom_[addr]) return;
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
    // unmapped port: write vanishes
}

uint16_t Chipset::io_in16(uint16_t port) {
    // The HDD data register is 16-bit at 0x1F0 alone; 0x1F1 is the Error register.
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
    for (uint32_t page = 0; page < mem_clocks.size(); ++page) {
        uint32_t a = page << 12;
        bool board = a < 0xA0000 || (a >= 0xF0000 && a < 0x100000) || a >= kRomAlias;
        mem_clocks[page] = board ? kClocks16 : kClocks8;  // EGA, its ROM and empty slots are 8-bit
        if (a >= 0xA0000 && a < 0xC0000) mem_clocks[page] |= cpu80286::Bus::kMemSlotted;
    }
    bus.mem_clocks = mem_clocks.data();
    bus.mem_wait = [this](uint32_t addr, uint64_t now) { return ega.cpu_access_clocks(addr, now); };
    bus.io_clocks = io_clocks;
    bus.read = [this](uint32_t addr) { return mem_read(addr); };
    bus.write = [this](uint32_t addr, uint8_t v) { mem_write(addr, v); };
    bus.in = [this](uint16_t port) { return io_in(port); };
    bus.out = [this](uint16_t port, uint8_t v) { io_out(port, v); };
    bus.in16 = [this](uint16_t port) { return io_in16(port); };
    bus.out16 = [this](uint16_t port, uint16_t v) { io_out16(port, v); };
    return bus;
}

void Chipset::tick(uint64_t cpu_cycles, double cpu_hz) {
    int refreshes = 0;
    int ch0_rises = pit.tick(cpu_cycles, cpu_hz, &refreshes);
    for (int i = 0; i < ch0_rises; ++i) pic_master.raise(0);
    // Port 61h bit 4 toggles with each refresh request (IBM PC/AT Technical Reference, port 61h).
    if (refreshes & 1) refresh_toggle_ = !refresh_toggle_;
    held_clocks_ += refreshes * kRefreshClocks;
    speaker.update(cpu_cycles, (port61_ & 0x02) != 0, pit.channel2_output());

    fdc.tick(cpu_cycles);
    ega.tick(cpu_cycles);
    // Paced FDC transfers copy the whole block at once using DMA1 channel 2's
    // programmed address/count/page. DMA bypasses the A20 gate, so this indexes mem directly.
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
        held_clocks_ += int(len) * kDmaCycleClocks;
        fdc.finish_transfer(len);
    }
    // IRQ6 is edge-triggered.
    bool fdc_irq_now = fdc.irq_pending();
    if (fdc_irq_now && !fdc_irq_prev_) pic_master.raise(6);
    fdc_irq_prev_ = fdc_irq_now;

    // IRQ1, edge-triggered.
    bool kbc_irq_now = kbc.irq1_pending();
    if (kbc_irq_now && !kbc_irq_prev_) pic_master.raise(1);
    kbc_irq_prev_ = kbc_irq_now;

    // The EGA's IRQ2 pin reaches the AT's slave line 1 (IRQ9).
    bool ega_irq_now = ega.vertical_interrupt();
    if (ega_irq_now && !ega_irq_prev_) pic_slave.raise(1);
    ega_irq_prev_ = ega_irq_now;

    // IRQ14 is slave line 6.
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
        pic_master.acknowledge();  // master INTA side effects; vector discarded
        return pic_slave.acknowledge();
    }
    return pic_master.acknowledge();
}

}  // namespace ibmpcat
