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
    update_irq_lines();
    // mem, rom_ and cmos survive reset: RAM and CMOS are not cleared by a warm reset.
}

void Chipset::load_rom(uint32_t addr, const uint8_t *data, std::size_t len) {
    std::memcpy(mem.data() + addr, data, len);
    for (std::size_t i = 0; i < len; ++i) rom_[addr + i] = true;
}

uint8_t Chipset::mem_read(uint32_t addr) {
    if (!kbc.a20_enabled()) addr &= ~0x100000u;  // the gate holds A20 low, nothing else
    return bus_read(addr);
}
void Chipset::mem_write(uint32_t addr, uint8_t v) {
    if (!kbc.a20_enabled()) addr &= ~0x100000u;
    bus_write(addr, v);
}
uint8_t Chipset::bus_read(uint32_t addr) {
    if (ega.owns_mem(addr)) return ega.mem_read(addr);
    if (addr >= kRomAlias) return is_rom(addr) ? mem[addr - kRomAliasOffset] : 0xFF;
    if (addr >= mem.size()) return 0xFF;      // unpopulated
    return mem[addr];
}
void Chipset::bus_write(uint32_t addr, uint8_t v) {
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
    // Each rising edge sets IRR even if OUT fell again inside this step.
    for (int i = 0; i < ch0_rises; ++i) {
        pic_master.set_line(0, false);
        pic_master.set_line(0, true);
    }
    // Port 61h bit 4 toggles with each refresh request (IBM PC/AT Technical Reference, port 61h).
    if (refreshes & 1) refresh_toggle_ = !refresh_toggle_;
    held_clocks_ += refreshes * kRefreshClocks;
    speaker.update(cpu_cycles, (port61_ & 0x02) != 0, pit.channel2_output());

    fdc.tick(cpu_cycles);
    ega.tick(cpu_cycles);
    run_fdc_dma();
    hdd.tick(cpu_cycles);
    update_irq_lines();
}

void Chipset::update_irq_lines() {
    pic_master.set_line(0, pit.channel0_output());
    pic_master.set_line(1, kbc.irq1_pending());
    pic_master.set_line(6, fdc.irq_pending());
    // The EGA drives bus IRQ2, which the AT routes to IRQ9.
    pic_slave.set_line(1, ega.vertical_interrupt());
    pic_slave.set_line(6, hdd.irq_pending());
    sync_cascade();
}

void Chipset::run_fdc_dma() {
    bool drq = fdc.transfer_ready() && fdc.dma_enabled();
    dma1.set_dreq(2, drq);
    // DMA1's HRQ reaches the CPU only through DMA2 channel 0 in cascade mode (system channel 4).
    bool hold = dma2.can_service(0) && (dma2.mode(0) >> 6) == Dma8237::kCascadeMode;
    if (!drq || !hold || !dma1.can_service(2)) return;
    std::size_t len = fdc.transfer_length();
    uint8_t* img = fdc.transfer_image_ptr();
    Dma8237::Type type = dma1.type(2);
    bool to_mem = type == Dma8237::kWrite && !fdc.transfer_is_write();
    bool from_mem = type == Dma8237::kRead && fdc.transfer_is_write();
    uint32_t page = uint32_t(dma1.page(2)) << 16;
    std::size_t n = 0;
    bool tc = false;
    while (n < len && !tc) {
        uint32_t addr = page | dma1.transfer(2, &tc);
        if (img != nullptr) {
            if (to_mem) bus_write(addr, img[n]);
            else if (from_mem) img[n] = bus_read(addr);
        }
        ++n;
    }
    held_clocks_ += int(n) * kDmaCycleClocks;
    fdc.finish_transfer(n);
    dma1.set_dreq(2, false);
}

int Chipset::poll_interrupt() {
    sync_cascade();
    if (!pic_master.has_interrupt()) return -1;
    int level = pic_master.inta1();
    uint8_t vec;
    if (pic_master.cascades(level)) {
        // Only the slave whose ID matches the CAS lines drives the bus.
        if (pic_slave.slave_id() == level) {
            int slave_level = pic_slave.inta1();
            vec = pic_slave.inta2(slave_level);
        } else {
            vec = 0xFF;
        }
        pic_master.inta2(level);
    } else {
        vec = pic_master.inta2(level);
    }
    // INT goes inactive after the second INTA (8259A data sheet, "Interrupt Sequence").
    pic_master.set_line(2, false);
    sync_cascade();
    return vec;
}

}  // namespace ibmpcat
