#include "chipset.h"

#include <cstring>

namespace pc486 {

namespace {
// ISA DMA page-register ports (74-series glue, not part of the 8237).
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
    next_service_ = 0;
    port61_ = 0;
    refresh_toggle_ = false;
    pic_master.reset();
    pic_slave.reset();
    pit.reset();
    kbc.reset();
    dma1.reset();
    dma2.reset();
    fdc.reset();
    vga.reset();
    hdd.reset();
    cdrom.reset();
    speaker.reset();
    sb.reset();
    mpu.reset();
    update_irq_lines();
    note_a20();  // kbc.reset() closes the gate again
    // mem, rom_ and cmos survive reset.
}

void Chipset::set_cpu_hz(double hz) {
    fdc.set_cpu_hz(hz);
    hdd.set_cpu_hz(hz);
    cdrom.set_cpu_hz(hz);
    sb.set_cpu_hz(hz);
    vga.set_cpu_hz(hz);
}

void Chipset::load_rom(uint32_t addr, const uint8_t *data, std::size_t len) {
    std::memcpy(mem.data() + addr, data, len);
    for (std::size_t i = 0; i < len; ++i) rom_[addr + i] = true;
    for (std::size_t i = 0; i < len; ++i) rom_page_[(addr + i) >> 12] = true;
    ++map_epoch_;
}

uint8_t Chipset::mem_read(uint32_t addr) {
    if (!kbc.a20_enabled()) addr &= 0xFFFFF;  // gate closed: real 20-bit wraparound, regardless of installed RAM
    addr = rom_alias(addr);
    if (vga.owns_mem(addr)) return vga.mem_read(addr);
    if (addr >= mem.size()) return 0xFF;      // nothing populated up there
    return mem[addr];
}
void Chipset::mem_write(uint32_t addr, uint8_t v) {
    if (!kbc.a20_enabled()) addr &= 0xFFFFF;
    addr = rom_alias(addr);
    if (vga.owns_mem(addr)) { vga.mem_write(addr, v); return; }
    if (addr >= mem.size()) return;
    if (rom_[addr]) return;  // ROM: writes ignored, matching real hardware
    mem[addr] = v;
}

uint8_t *Chipset::page_host(uint32_t page_base, bool write) {
    note_vga_mapping();
    // A20 masks a page base like a byte address.
    if (!kbc.a20_enabled()) page_base &= 0xFFFFF;
    page_base = rom_alias(page_base);
    if (vga.owns_mem(page_base)) {
        // Mode 13h's aperture is linear, so hand the CPU a pointer into VRAM and skip
        // the per-byte plane decode.
        return vga.linear_page(page_base, write);
    }
    if (page_base >= mem.size()) return nullptr;  // nothing populated up there
    if (write && rom_page_[page_base >> 12]) return nullptr;
    return mem.data() + page_base;
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
    // Any port access can change device state, so reopen the service gate.
    next_service_ = 0;
    if (pic_master.owns(port)) return pic_master.in(port);
    if (pic_slave.owns(port)) return pic_slave.in(port);
    if (pit.owns(port)) return pit.in(port);
    if (kbc.owns(port)) return kbc.in(port);
    if (kbc.owns_fast_a20(port)) return kbc.fast_a20_in();
    if (cmos.owns(port)) return cmos.in(port);
    if (dma1.owns(port)) return dma1.in(port);
    if (dma2.owns(port)) return dma2.in(port);
    if (fdc.owns(port)) return fdc.in(port);
    if (vga.owns_port(port)) return vga.in(port);
    if (hdd.owns(port)) return hdd.in(port);
    if (cdrom.owns(port)) return cdrom.in(port);
    if (sb.owns(port)) return sb.in(port);
    if (mpu.owns(port)) return mpu.in(port);
    if (port == 0x61) return port61();
    int controller, channel;
    if (page_port_map(port, controller, channel)) return (controller == 1 ? dma1 : dma2).page(channel);
    return 0xFF;
}
void Chipset::io_out(uint16_t port, uint8_t v) {
    io_out_impl(port, v);
    // A VGA register write can change the aperture mapping; recheck before the
    // guest uses a pointer resolved under the old mode.
    note_vga_mapping();
}

void Chipset::io_out_impl(uint16_t port, uint8_t v) {
    next_service_ = 0;
    if (pic_master.owns(port)) { pic_master.out(port, v); return; }
    if (pic_slave.owns(port)) { pic_slave.out(port, v); return; }
    if (pit.owns(port)) { pit.out(port, v); return; }
    if (kbc.owns(port)) { kbc.out(port, v); note_a20(); return; }
    if (kbc.owns_fast_a20(port)) { kbc.fast_a20_out(v); note_a20(); return; }
    if (cmos.owns(port)) { cmos.out(port, v); return; }
    if (dma1.owns(port)) { dma1.out(port, v); return; }
    if (dma2.owns(port)) { dma2.out(port, v); return; }
    if (fdc.owns(port)) { fdc.out(port, v); return; }
    if (vga.owns_port(port)) { vga.out(port, v); return; }
    if (hdd.owns(port)) { hdd.out(port, v); return; }
    if (cdrom.owns(port)) { cdrom.out(port, v); return; }
    if (sb.owns(port)) { sb.out(port, v); return; }
    if (mpu.owns(port)) { mpu.out(port, v); return; }
    if (port == 0x61) { set_port61(v); return; }
    if (port == 0x80) { last_post_code_ = v; return; }
    if (port == 0xE9) { debug_console_.push_back(char(v)); return; }
    int controller, channel;
    if (page_port_map(port, controller, channel)) { (controller == 1 ? dma1 : dma2).set_page(channel, v); return; }
    // unmapped port -- real hardware: write vanishes (open bus)
}

uint16_t Chipset::io_in16(uint16_t port) {
    next_service_ = 0;
    // IDE data registers are 16-bit at a single port.
    if (port == 0x1F0) return hdd.data_in16();
    if (port == 0x170) return cdrom.data_in16();
    // SVGA extension index/data ports are 16-bit too.
    if (Ega::owns_port16(port)) return vga.in16(port);
    return uint16_t(io_in(port)) | (uint16_t(io_in(uint16_t(port + 1))) << 8);
}
void Chipset::io_out16(uint16_t port, uint16_t v) {
    next_service_ = 0;
    if (port == 0x1F0) { hdd.data_out16(v); return; }
    if (port == 0x170) { cdrom.data_out16(v); return; }
    if (Ega::owns_port16(port)) { vga.out16(port, v); return; }
    io_out(port, uint8_t(v & 0xFF));
    io_out(uint16_t(port + 1), uint8_t(v >> 8));
}

cpu80486::Bus Chipset::make_bus() {
    // Chipset names the six bus operations exactly as Bus::For expects.
    return cpu80486::Bus::For(this);
}

void Chipset::service(uint64_t cpu_cycles, double cpu_hz) {
    int refreshes = 0;
    int ch0_rises = pit.tick(cpu_cycles, cpu_hz, &refreshes);
    // Each rising edge sets IRR even if OUT fell again inside this step.
    for (int i = 0; i < ch0_rises; ++i) {
        pic_master.set_line(0, false);
        pic_master.set_line(0, true);
    }
    // Port 61h bit 4 toggles with each refresh request (IBM PC/AT Technical Reference, port 61h).
    if (refreshes & 1) refresh_toggle_ = !refresh_toggle_;
    speaker.update(cpu_cycles, (port61_ & 0x02) != 0, pit.channel2_output());

    fdc.tick(cpu_cycles);
    vga.tick(cpu_cycles);
    // Once the paced FDC transfer wait has elapsed, copy the block in one step
    // through DMA1 channel 2. DMA bypasses the A20 gate.
    dma1.set_dreq(2, fdc.transfer_ready());
    if (fdc.transfer_ready() && !dma1.channel_masked(2)) {
        uint16_t dma_len16 = uint16_t(dma1.count(2) + 1);  // 8237 count register is programmed as N-1
        std::size_t len = std::min(fdc.transfer_length(), std::size_t(dma_len16));
        uint32_t addr = (uint32_t(dma1.page(2)) << 16) | dma1.address(2);
        uint8_t *img = fdc.transfer_image_ptr();
        if (img != nullptr) {
            if (fdc.transfer_is_write()) {
                for (std::size_t i = 0; i < len; ++i) img[i] = mem[(addr + i) & (kRamSize - 1)];
            } else {
                for (std::size_t i = 0; i < len; ++i) mem[(addr + i) & (kRamSize - 1)] = img[i];
            }
        }
        // The block lands at once, so its bus holds do too.
        for (std::size_t i = 0; i < len; ++i)
            dma_timing(uint32_t(addr + i) & (kRamSize - 1), 1, !fdc.transfer_is_write());
        bool tc;
        for (std::size_t i = 0; i < len; ++i) dma1.transfer(2, &tc);
        fdc.finish_transfer(len);
    }

    kbc.tick(cpu_cycles, cpu_hz);
    sb.tick(cpu_cycles);
    // DREQ follows the jumpered channel every tick so a masked channel shows a pending request.
    {
        bool is16 = sb.transfer_is_16bit();
        int global_ch = sb.transfer_dma_channel();
        Dma8237 &dma = is16 ? dma2 : dma1;
        int idx = is16 ? (global_ch - 4) : global_ch;
        if (idx >= 0 && idx <= 3) dma.set_dreq(idx, sb.transfer_ready());
    }
    // Skips the out-of-line call unless a block is waiting.
    if (sb.transfer_ready()) service_sb_dma();
    hdd.tick(cpu_cycles);
    cdrom.tick(cpu_cycles);
    cmos.tick(cpu_cycles, cpu_hz);
    update_irq_lines();

    next_service_ = cpu_cycles + pit.cycles_to_next_count();
}

void Chipset::update_irq_lines() {
    pic_master.set_line(0, pit.channel0_output());
    // A byte landing drops the line for a moment first, as the 8042 does
    // between bytes, so a refill inside one 0x60 read still makes an edge.
    if (kbc.irq1_pending()) { pic_master.set_line(1, false); kbc.clear_irq1(); }
    if (kbc.irq12_pending()) { pic_slave.set_line(4, false); kbc.clear_irq12(); }
    pic_master.set_line(1, kbc.irq1_line());
    pic_slave.set_line(4, kbc.irq12_line());
    pic_master.set_line(6, fdc.irq_pending());
    // Mixer register 80h picks the SB16's line (SBPG 2-6). Its "IRQ2" is bus
    // IRQ2, which the AT routes to IRQ9 (IBM 5170 Technical Reference).
    int sb_line = sb.irq_pending() ? sb.irq_line() : -1;
    pic_master.set_line(5, sb_line == 5);
    pic_master.set_line(7, sb_line == 7);
    pic_slave.set_line(1, sb_line == 2);
    pic_slave.set_line(2, sb_line == 10);
    // The MC146818's active-low IRQ is inverted onto IRQ8.
    pic_slave.set_line(0, cmos.irq_pending());
    pic_slave.set_line(6, hdd.irq_pending());
    pic_slave.set_line(7, cdrom.irq_pending());
    sync_cascade();
}

void Chipset::service_sb_dma() {
    if (!sb.transfer_ready()) return;
    bool is16 = sb.transfer_is_16bit();
    int global_ch = sb.transfer_dma_channel();
    Dma8237 &dma = is16 ? dma2 : dma1;
    int idx = is16 ? (global_ch - 4) : global_ch;
    if (idx < 0 || idx > 3 || dma.channel_masked(idx)) { sb.finish_transfer(0); return; }

    uint8_t *buf = sb.transfer_buffer();
    std::size_t want = sb.transfer_length();  // already in bytes
    // Direction follows the DSP A/D bit: input (recording) writes memory, playback reads it.
    // The address is recomputed from the live page/address registers each unit,
    // so 64KB page wrap and decrement mode (mode bit 5) work (8237 behavior).
    std::size_t moved = 0;
    if (is16) {
        // 16-bit channel: A1-A16 from the address register, A17-A23 from the page
        // register, and count/advance step in words.
        std::size_t avail_bytes = (std::size_t(dma.count(idx)) + 1) * 2;
        std::size_t len = std::min(want, avail_bytes) & ~std::size_t(1);  // whole words only
        for (std::size_t i = 0; i < len; i += 2) {
            uint32_t phys = (uint32_t(dma.page(idx)) << 16) | (uint32_t(dma.address(idx)) << 1);
            dma_timing(phys & (kRamSize - 1), 2, sb.transfer_is_input());
            if (sb.transfer_is_input()) {
                mem[phys & (kRamSize - 1)] = buf[i];
                mem[(phys + 1) & (kRamSize - 1)] = buf[i + 1];
            } else {
                buf[i] = mem[phys & (kRamSize - 1)];
                buf[i + 1] = mem[(phys + 1) & (kRamSize - 1)];
            }
            bool tc;
            dma.transfer(idx, &tc);
        }
        moved = len;
    } else {
        std::size_t avail_bytes = std::size_t(dma.count(idx)) + 1;
        std::size_t len = std::min(want, avail_bytes);
        for (std::size_t i = 0; i < len; ++i) {
            uint32_t phys = (uint32_t(dma.page(idx)) << 16) | dma.address(idx);
            dma_timing(phys & (kRamSize - 1), 1, sb.transfer_is_input());
            if (sb.transfer_is_input()) mem[phys & (kRamSize - 1)] = buf[i];
            else buf[i] = mem[phys & (kRamSize - 1)];
            bool tc;
            dma.transfer(idx, &tc);
        }
        moved = len;
    }
    sb.finish_transfer(moved);
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

}  // namespace pc486
