// Midway Galaga (1981) board: three Z80s, Namco 06XX/51XX/54XX, WSG.
//
// Memory map is the CPU-board decode shared by all three CPUs (private ROM
// at $0000–$3FFF). coinop.org GalagaMap and the MAME galaga.cpp header are
// cross-checks. Clocks: 18.432 MHz master, each Z80 at 18.432/6 = 3.072 MHz.
// 51XX (MB8843) and 54XX (MB8844) run their mask ROM when the user supplied
// a 1024-byte image; otherwise the external command protocol is answered
// on the 06XX bus (a labelled departure, reported by the page).

#ifndef GALAGA_MACHINE_H
#define GALAGA_MACHINE_H

#include "cpu_z80.h"
#include "mb88.h"
#include "namco/wsg.h"
#include "video.h"

#include <array>
#include <cstdint>
#include <vector>

namespace galaga {

constexpr int kWatchdogFrames = 8;
constexpr int kMcuRomBytes = Mb88::kRomBytes;

struct Inputs {
    // 1 = pressed. The 51XX R ports see the contacts active-low.
    uint8_t in0 = 0;  // bit1 right, bit3 left (cocktail bits 5 and 7 unmapped)
    uint8_t in1 = 0;  // fire, starts, coins, service
    // Midway factory defaults. MAME galagamw INPUT_PORTS is a cross-check
    // of the switch bits; the labels are the Midway manual's.
    uint8_t dsw_a = 0xF7;  // physical SWB. Bit 6 is unused and open.
    uint8_t dsw_b = 0x97;  // physical SWA: 1C/1C, 20k/70k, 3 lives
};

struct RomSet {
    std::array<uint8_t, 0x4000> main{};
    std::array<uint8_t, 0x1000> sub{};
    std::array<uint8_t, 0x1000> sound{};
    std::array<uint8_t, 0x1000> tiles{};
    std::array<uint8_t, 0x2000> sprites{};
    std::array<uint8_t, 32> palette{};
    std::array<uint8_t, 256> char_lut{};
    std::array<uint8_t, 256> sprite_lut{};
    std::array<uint8_t, 256> wave{};
    std::array<uint8_t, kMcuRomBytes> mcu51{};
    std::array<uint8_t, kMcuRomBytes> mcu54{};
    bool has51 = false;
    bool has54 = false;
};

class Machine {
public:
    Inputs inputs;
    z80::Cpu main;
    z80::Cpu sub;
    z80::Cpu sound;
    Video video;
    namco::Wsg wsg;
    std::array<uint8_t, 0x4000> rom_main{};
    std::array<uint8_t, 0x1000> rom_sub{};
    std::array<uint8_t, 0x1000> rom_sound{};
    std::array<uint8_t, 0x400> ram1{};
    std::array<uint8_t, 0x400> ram2{};
    std::array<uint8_t, 0x400> ram3{};
    bool irq1_enable = false;
    bool irq2_enable = false;
    bool nmi_disable = false;
    bool sub_reset = true;
    bool sound_reset = true;
    bool mcu51_hle = true;
    bool mcu54_hle = true;
    int watchdog_ = kWatchdogFrames;
    int frames = 0;
    int credits = 0;
    std::vector<float> audio;
    int audio_hz = 48000;
    bool watchdog_reset = false;

    Machine();
    void reset();
    void load_roms(const RomSet& set);
    int run_cycles(int n);
    void render(uint32_t* upright) const;

    uint8_t mem_read(int cpu, uint16_t addr);
    void mem_write(int cpu, uint16_t addr, uint8_t v);
    uint8_t mem_read(uint16_t addr) { return mem_read(0, addr); }
    void mem_write(uint16_t addr, uint8_t v) { mem_write(0, addr, v); }

private:
    Mb88 mcu51_;
    Mb88 mcu54_;
    z80::Bus make_bus(int cpu);
    int step_cpu(int cpu);
    void on_vblank();
    void service_mcu(int z80_cycles);
    uint8_t io06_read();
    void io06_write(uint8_t v);
    void io06_ctrl(uint8_t v);
    void mcu_select(bool level);
    void mcu51_command(uint8_t data);
    uint8_t mcu51_hle_read();
    uint8_t r_nibble(int n) const;
    void note_coins();

    uint8_t io_ctrl_ = 0;
    int io_div_count_ = 0;
    bool io_stretch_ = false;
    bool io_phase_ = false;
    int mcu51_mode_ = 0;
    int mcu51_args_ = 0;
    int mcu51_read_i_ = 0;
    uint8_t mcu51_coinage_[4]{};
    uint8_t prev_in1_ = 0;
    int mcu_acc_ = 0;
    int mcu54_args_ = 0;
    int noise_left_ = 0;
    int noise_amp_ = 0;
    int noise_vol_ = 15;
    uint32_t noise_lfsr_ = 1;
    bool irq1_line_ = false;
    bool irq2_line_ = false;
    double audio_acc_ = 0;
};

}  // namespace galaga

#endif
