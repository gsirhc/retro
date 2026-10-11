// Installs DOOM 1.9 shareware onto the DOOM preset C: by running id's own INSTALL.BAT
// (DEICE, then the DOOMS_19.EXE self-extractor, then SETUP) on this emulator, from the
// library's two install floppies, with injected keys and screen-text matching.
//
// Usage: build_doom_hdd <bios> <vgabios> <base.img> <disk1.img> <disk2.img> <out.img> [max_cycles]
//
// <base.img> is build-doom-base.sh's bare FreeDOS C:. PC486_TRACE=1 dumps every screen
// change to stderr.

#include "../machine.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace pc486;

namespace {

std::vector<uint8_t> ReadFile(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Planar-VRAM text read as in build_freedos_hdd.cpp: plane 0, following the CRTC start address
std::string ScreenText(Machine &m) {
    const auto &vga = m.chipset.vga;
    std::string out;
    for (int row = 0; row < 25; ++row) {
        std::string line;
        for (int col = 0; col < 80; ++col) {
            uint32_t plane_off = (uint32_t(vga.start_offset()) + uint32_t(row * 80 + col)) & 0xFFFF;
            uint8_t ch = vga.vram[(plane_off << 2) + 0];
            line.push_back((ch >= 32 && ch < 127) ? char(ch) : ' ');
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line;
        out += '\n';
    }
    return out;
}

// Text of the cells drawn in `attr`, the menu highlight bar
std::string CellsWithAttr(Machine &m, uint8_t attr) {
    const auto &vga = m.chipset.vga;
    std::string out;
    for (int i = 0; i < 80 * 25; ++i) {
        uint32_t plane_off = (uint32_t(vga.start_offset()) + uint32_t(i)) & 0xFFFF;
        if (vga.vram[(plane_off << 2) + 1] != attr) continue;
        uint8_t ch = vga.vram[(plane_off << 2) + 0];
        out.push_back((ch >= 32 && ch < 127) ? char(ch) : ' ');
    }
    return out;
}

std::string Trim(const std::string &s) {
    std::size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// Set 1 make codes; the bool asks for Left Shift
std::pair<uint8_t, bool> Set1MakeCode(char c) {
    static const std::map<char, uint8_t> table = {
        {'1', 0x02}, {'2', 0x03}, {'3', 0x04}, {'4', 0x05}, {'5', 0x06},
        {'6', 0x07}, {'7', 0x08}, {'8', 0x09}, {'9', 0x0A}, {'0', 0x0B},
        {'q', 0x10}, {'w', 0x11}, {'e', 0x12}, {'r', 0x13}, {'t', 0x14},
        {'y', 0x15}, {'u', 0x16}, {'i', 0x17}, {'o', 0x18}, {'p', 0x19},
        {'a', 0x1E}, {'s', 0x1F}, {'d', 0x20}, {'f', 0x21}, {'g', 0x22},
        {'h', 0x23}, {'j', 0x24}, {'k', 0x25}, {'l', 0x26}, {';', 0x27},
        {'\\', 0x2B}, {'z', 0x2C}, {'x', 0x2D}, {'c', 0x2E}, {'v', 0x2F},
        {'b', 0x30}, {'n', 0x31}, {'m', 0x32}, {'.', 0x34}, {'=', 0x0D},
        {' ', 0x39}, {'\n', 0x1C}, {'\x1b', 0x01},
    };
    if (c == ':') return {0x27, true};
    if (c >= 'A' && c <= 'Z') return {table.at(char(c - 'A' + 'a')), true};
    auto it = table.find(c);
    return {it != table.end() ? it->second : 0, false};
}

constexpr uint8_t kScanLShift = 0x2A;

// One gap per byte: the 8042 has one output register (build_freedos_hdd.cpp)
void Inject(Machine &m, uint8_t code) {
    m.chipset.kbc.inject_scancode(code);
    m.run_cycles(150000);
}

void SendKey(Machine &m, uint8_t make, bool shift = false) {
    if (shift) Inject(m, kScanLShift);
    Inject(m, make);
    Inject(m, uint8_t(make | 0x80));
    if (shift) Inject(m, uint8_t(kScanLShift | 0x80));
}

// DOOM SETUP's selection bar: bright white on grey
constexpr uint8_t kHighlight = 0x7F;

constexpr uint8_t kScanUp = 0x48, kScanDown = 0x50, kScanEnter = 0x1C, kScanEsc = 0x01, kScanF10 = 0x44;

// Steps run in order. One fires once `wait_for` is on screen, the screen has changed
// since the previous step's keys, and it has then been still for kSettle.
struct Step {
    std::string wait_for;
    std::string send;  // tokens: @ENTER @ESC @F10 @WAIT=seconds @PICK=Menu_entry @DISK1 @DISK2 @DONE, else literal keys
};

}  // namespace

int main(int argc, char **argv) {
    if (argc < 7) {
        std::fprintf(stderr,
            "usage: %s <bios> <vgabios> <base.img> <disk1.img> <disk2.img> <out.img> [max_cycles]\n",
            argv[0]);
        return 2;
    }
    uint64_t budget = argc > 7 ? std::strtoull(argv[7], nullptr, 10) : 200'000'000'000ull;
    const bool trace = std::getenv("PC486_TRACE") != nullptr;

    Machine m;
    m.reset();
    {
        auto bios = ReadFile(argv[1]);
        auto vga = ReadFile(argv[2]);
        if (bios.empty() || vga.empty()) { std::fprintf(stderr, "cannot open BIOS/VGABIOS\n"); return 2; }
        m.chipset.load_rom(0x100000 - uint32_t(bios.size()), bios.data(), bios.size());
        m.chipset.load_rom(0xC0000, vga.data(), vga.size());
    }
    auto hdd = ReadFile(argv[3]);
    auto disk1 = ReadFile(argv[4]);
    auto disk2 = ReadFile(argv[5]);
    if (hdd.size() != pc486::Wd1003::kImageBytes || disk1.empty() || disk2.empty()) {
        std::fprintf(stderr, "cannot open the base C: or the install floppies\n");
        return 2;
    }
    m.chipset.hdd.mount(0, hdd.data(), hdd.size());

    // Booting with A: empty, then inserting disk 1 at the prompt, keeps the BIOS on C:
    std::vector<Step> steps = {
        {"C:\\>", "@DISK1 a:\n"},
        {"A:\\>", "install\n"},
        {"Which drive to install to?", "c"},
        {"Enter directory name to copy into:", "@ENTER"},
        {"That directory does not exist! Create it?", "y"},
        {"Insert disk 2", "@DISK2 @ENTER"},
        {"Insert Disk 1 again", "@DISK1 @ENTER"},
        {"Controller Type", "@PICK=Keyboard_+_Mouse @ENTER"},
        {"Select Music Card", "@PICK=Sound_Blaster @ENTER"},
        {"Select Port Address", "@PICK=220 @ENTER"},
        {"Select Sound FX Device", "@PICK=Sound_Blaster @ENTER"},
        {"Select Port Address", "@PICK=220 @ENTER"},
        {"Select IRQ", "@PICK=5 @ENTER"},
        {"Select DMA", "@PICK=1 @ENTER"},
        {"Sound FX to mix", "@PICK=8 @ENTER"},
        // DOOM saves DEFAULT.CFG on quit, so one = there leaves the next run full width over the status bar
        {"Save parameters and launch DOOM", "@PICK=Save_parameters_and_launch_DOOM @ENTER"},
        {"DOOM System Startup", "@WAIT=15 = @WAIT=2 @F10 @WAIT=2 y"},
        {"C:\\DOOMS>", "@DONE"},
    };

    std::string last_screen, prev_screen, sent_screen;
    std::size_t step_idx = 0;
    bool done = false;
    uint64_t still_since = 0;
    const uint64_t kSettle = 130'000'000;  // ~2s emulated, also covers DOS's disk-swap timing check

    const uint64_t kChunk = 500'000;
    for (uint64_t used = 0; used < budget && !done; used += kChunk) {
        m.run_cycles(int64_t(kChunk));
        std::string s = ScreenText(m);
        last_screen = s;
        if (s != prev_screen) {
            still_since = m.total_cycles();
            if (trace) {
                std::fprintf(stderr, "\n===== screen @ %llu cycles =====\n%s",
                             (unsigned long long)m.total_cycles(), s.c_str());
                std::map<uint8_t, int> attrs;
                for (int i = 0; i < 80 * 25; ++i) {
                    uint32_t off = (uint32_t(m.chipset.vga.start_offset()) + uint32_t(i)) & 0xFFFF;
                    ++attrs[m.chipset.vga.vram[(off << 2) + 1]];
                }
                for (auto [a, n] : attrs)
                    if (n < 400) std::fprintf(stderr, "  attr %02X: \"%s\"\n", a, CellsWithAttr(m, a).c_str());
            }
        }
        prev_screen = s;

        const Step &st = steps[step_idx];
        if (s.find(st.wait_for) == std::string::npos) continue;
        if (s == sent_screen) continue;
        if (m.total_cycles() - still_since < kSettle) continue;

        std::fprintf(stderr, "[cyc %12llu] \"%s\" -> %s\n",
                     (unsigned long long)m.total_cycles(), st.wait_for.c_str(), st.send.c_str());
        std::size_t pos = 0;
        while (pos < st.send.size()) {
            std::size_t sp = st.send.find(' ', pos);
            std::string tok = st.send.substr(pos, sp == std::string::npos ? sp : sp - pos);
            pos = (sp == std::string::npos) ? st.send.size() : sp + 1;
            if (tok.empty()) continue;
            if (tok == "@DONE") { done = true; break; }
            if (tok == "@ENTER") SendKey(m, kScanEnter);
            else if (tok == "@F10") SendKey(m, kScanF10);
            else if (tok.rfind("@WAIT=", 0) == 0) m.run_cycles(int64_t(std::stoi(tok.substr(6))) * 66'000'000);
            else if (tok == "@ESC") SendKey(m, kScanEsc);
            else if (tok.rfind("@PICK=", 0) == 0) {
                std::string want = tok.substr(6);
                for (char &c : want) if (c == '_') c = ' ';
                int tries = 0;
                // Keypad arrows: SETUP ignores the E0-prefixed grey ones (INT 16h AL=E0h)
                while (Trim(CellsWithAttr(m, kHighlight)) != want && tries++ < 16) {
                    SendKey(m, tries <= 8 ? kScanUp : kScanDown);
                    m.run_cycles(20'000'000);
                }
                if (tries > 16) {
                    std::fprintf(stderr, "FAILED: no \"%s\" entry to highlight\n%s", want.c_str(), s.c_str());
                    return 1;
                }
            } else if (tok == "@DISK1" || tok == "@DISK2") {
                const auto &disk = tok == "@DISK1" ? disk1 : disk2;
                m.chipset.fdc.unmount(0);
                m.run_cycles(66'000'000);
                m.chipset.fdc.mount(0, disk.data(), disk.size());
                m.run_cycles(130'000'000);
            } else {
                for (char c : tok) {
                    auto [mk, shift] = Set1MakeCode(c);
                    if (mk) SendKey(m, mk, shift);
                }
            }
        }
        sent_screen = ScreenText(m);
        ++step_idx;
    }

    if (!done) {
        std::fprintf(stderr,
            "FAILED: cycle budget (%llu) exhausted at step %zu of %zu (\"%s\"). Last screen:\n%s\n",
            (unsigned long long)budget, step_idx + 1, steps.size(),
            steps[step_idx].wait_for.c_str(), last_screen.c_str());
        return 1;
    }

    std::vector<uint8_t> final_image = m.chipset.hdd.image(0);
    std::ofstream out(argv[6], std::ios::binary);
    out.write(reinterpret_cast<const char *>(final_image.data()), std::streamsize(final_image.size()));
    if (!out) { std::fprintf(stderr, "failed to write %s\n", argv[6]); return 2; }
    std::fprintf(stderr, "wrote %s at cycle %llu\n", argv[6], (unsigned long long)m.total_cycles());
    return 0;
}
