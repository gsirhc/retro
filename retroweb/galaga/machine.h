// Midway Galaga (1981) board: three Z80s, Namco 06XX/51XX/54XX, WSG.
//
// CPU-board decode shared by all three Z80s (coinop.org GalagaMap, Midway CPU-board schematic), 3.072 MHz each.
// 51XX/54XX run their mask ROM if the user supplies a 1024-byte image; otherwise the
// 06XX bus answers the command protocol directly (a labelled departure).

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
    // Midway manual figure 3. $6800 d0 is DIP 6K, d1 is DIP 6J (CPU-board schematic).
    uint8_t dsw_a = 0xF7;  // DIP 6J. Switch 7 is not used and open.
    uint8_t dsw_b = 0x97;  // DIP 6K: 1C/1C, 20k/70k, 3 lives
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

// Two-state RC network stepped by the trapezoidal rule: x' = A x + B u.
struct Linear2 {
    double m[2][2]{}, n[2]{};
    void step(double* x, double u_prev, double u) const;
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
    std::array<int, 2> coin_counter{};
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
    uint8_t dac54(int ch) const { return dac54_[ch]; }
    bool io06_rw() const { return io_rw_; }
    int mcu51_pc() const { return mcu51_.pc_full(); }
    void mem_write(uint16_t addr, uint8_t v) { mem_write(0, addr, v); }

private:
    void watchdog_fire();
    Mb88 mcu51_;
    Mb88 mcu54_;
    z80::Bus make_bus(int cpu);
    int step_cpu(int cpu);
    void on_vblank();
    void service_mcu();
    void io06_edge();
    uint8_t io06_read();
    void io06_write(uint8_t v);
    void io06_ctrl(uint8_t v);
    void mcu_select(bool level);
    void mcu51_command(uint8_t data);
    uint8_t mcu51_hle_read();
    uint8_t r_nibble(int n) const;
    void note_coins();
    void coin_counter_w(int n, bool on);
    bool coin_line_[2]{};

    uint8_t io_ctrl_ = 0;
    uint64_t io_next_edge_ = 0;
    bool io_stretch_ = false;
    bool io_timer_state_ = false;
    bool io_rw_ = false;
    int mcu51_mode_ = 0;
    int mcu51_args_ = 0;
    int mcu51_read_i_ = 0;
    uint8_t mcu51_coinage_[4]{};
    int mcu51_coins_[2]{};
    bool mcu51_started_ = false;
    uint8_t prev_in1_ = 0;
    uint64_t mcu_next_ = 0;
    int mcu51_wait_ = 0;
    int mcu54_wait_ = 0;
    int mcu54_args_ = 0;
    uint8_t mcu54_cmd_ = 0;
    struct Section {
        double x[2]{};
        double u = 0;
    };
    uint8_t dac54_[3]{};
    Section mfb_[2];
    Section amp_in_;
    Linear2 amp_disc_;
    int amp_hz_ = 0;
    double wsg_lp_ = 0;
    double wsg_in_ = 0;
    bool net_settled_ = false;
    double net_sum_ = 0;
    int net_n_ = 0;
    int net_clock_ = 0;
    void settle_sound_network();
    void step_54xx_network();
    float sound_output(float wsg);
    int noise_left_ = 0;
    int noise_amp_ = 0;
    int noise_vol_ = 15;
    uint32_t noise_lfsr_ = 1;
    double audio_acc_ = 0;
    int sub_credit_ = 0;
    int sound_credit_ = 0;
};

}  // namespace galaga

#endif
