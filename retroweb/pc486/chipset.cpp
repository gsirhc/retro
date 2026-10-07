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
    fdc_irq_prev_ = false;
    hdd_irq_prev_ = false;
    cdrom_irq_prev_ = false;
    sb_irq_prev_ = false;
    rtc_irq_prev_ = false;
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
    int ch0_rises = pit.tick(cpu_cycles, cpu_hz);
    for (int i = 0; i < ch0_rises; ++i) pic_master.raise(0);
    refresh_toggle_ = !refresh_toggle_;
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
        for (std::size_t i = 0; i < len; ++i) dma1.advance(2);
        fdc.finish_transfer(len);
    }
    // Edge-triggered: raise only on the 0->1 transition.
    bool fdc_irq_now = fdc.irq_pending();
    if (fdc_irq_now && !fdc_irq_prev_) pic_master.raise(6);
    fdc_irq_prev_ = fdc_irq_now;

    // IRQ1 and IRQ12 are level-checked, not edge-detected. i8042's in(0x60)
    // refills the output register inside the call that cleared it, so a queued
    // second byte never shows an edge and its interrupt would be lost.
    kbc.tick(cpu_cycles, cpu_hz);
    if (kbc.irq1_pending()) { pic_master.raise(1); kbc.clear_irq1(); }
    if (kbc.irq12_pending()) { pic_slave.raise(4); kbc.clear_irq12(); }

    // IRQ5 (Sound Blaster) is edge-triggered, one per completed block/command.
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
    bool sb_irq_now = sb.irq_pending();
    if (sb_irq_now && !sb_irq_prev_) {
        // Mixer register 80h picks the line (SBPG 2-6); -1 means none.
        switch (sb.irq_line()) {
            case 2:
                // "IRQ2" is wired to global IRQ9 (slave IR1) since the AT cascade takes master
                // IR2 (IBM 5170 Technical Reference).
                pic_slave.raise(1);
                break;
            case 5: pic_master.raise(5); break;
            case 7: pic_master.raise(7); break;
            case 10: pic_slave.raise(2); break;  // global IRQ10 = slave line 2
            default: break;                      // no line selected
        }
    }
    sb_irq_prev_ = sb_irq_now;

    // IRQ14 (hard disk) and IRQ15 (CD-ROM) are slave lines 6 and 7.
    hdd.tick(cpu_cycles);
    bool hdd_irq_now = hdd.irq_pending();
    if (hdd_irq_now && !hdd_irq_prev_) pic_slave.raise(6);
    hdd_irq_prev_ = hdd_irq_now;

    cdrom.tick(cpu_cycles);
    bool cdrom_irq_now = cdrom.irq_pending();
    if (cdrom_irq_now && !cdrom_irq_prev_) pic_slave.raise(7);
    cdrom_irq_prev_ = cdrom_irq_now;

    // IRQ8 (RTC, slave line 0): inverted active-low output, so a new flag is a rising edge.
    cmos.tick(cpu_cycles, cpu_hz);
    bool rtc_irq_now = cmos.irq_pending();
    if (rtc_irq_now && !rtc_irq_prev_) pic_slave.raise(0);
    rtc_irq_prev_ = rtc_irq_now;

    next_service_ = cpu_cycles + pit.cycles_to_next_count();
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
            dma.advance(idx);
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
            dma.advance(idx);
        }
        moved = len;
    }
    sb.finish_transfer(moved);
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

}  // namespace pc486
