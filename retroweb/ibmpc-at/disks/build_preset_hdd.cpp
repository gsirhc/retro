// Installs a shareware game onto a preset C: by running its publisher's own INSTALL.BAT
// on this emulator from the library's install floppy, with injected keys and
// screen-text matching.
//
// Usage: build_preset_hdd <keen1> <bios> <egabios> <base.img> <out.img> <disk1.img>
//
// <base.img> is build-preset-hdd.sh's bare FreeDOS C:. PC_AT_TRACE=1 dumps every screen
// change to stderr; PC_AT_BUDGET caps the run in cycles.

#include "../machine.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace ibmpcat;

namespace {

constexpr uint64_t kHz = 8'000'000;

std::vector<uint8_t> ReadFile(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Planar-VRAM text read as in build_freedos_hdd.cpp
std::string ScreenText(Machine &m) {
    const auto &ega = m.chipset.ega;
    std::string out;
    for (int row = 0; row < 25; ++row) {
        std::string line;
        for (int col = 0; col < 80; ++col) {
            uint32_t plane_off = ega.display_address((uint32_t(ega.start_offset()) + uint32_t(row * 80 + col)) & 0xFFFF, 0);
            uint8_t ch = ega.vram[(plane_off << 2) + 0];
            line.push_back((ch >= 32 && ch < 127) ? char(ch) : ' ');
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line;
        out += '\n';
    }
    return out;
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
        {'b', 0x30}, {'n', 0x31}, {'m', 0x32}, {'.', 0x34},
        {' ', 0x39}, {'\n', 0x1C},
    };
    if (c == ':') return {0x27, true};
    if (c >= 'A' && c <= 'Z') return {table.at(char(c - 'A' + 'a')), true};
    auto it = table.find(c);
    return {it != table.end() ? it->second : 0, false};
}

constexpr uint8_t kScanLShift = 0x2A, kScanEnter = 0x1C, kScanBackspace = 0x0E;

// One gap per byte: the 8042 has one output register
void Inject(Machine &m, uint8_t code) {
    m.chipset.kbc.inject_scancode(code);
    m.run_cycles(kHz / 400);
}

void SendKey(Machine &m, uint8_t make, bool shift = false) {
    if (shift) Inject(m, kScanLShift);
    Inject(m, make);
    m.run_cycles(kHz / 8);
    Inject(m, uint8_t(make | 0x80));
    if (shift) Inject(m, uint8_t(kScanLShift | 0x80));
}

// Steps run in order. One fires once `wait_for` is on screen, the screen differs from
// the one the previous step answered, and it has then been still for kSettle.
struct Step {
    std::string wait_for;
    std::string send;  // tokens: @ENTER @BS @DISK1 @DONE, else literal keys
};

// Booting with A: empty, then inserting the disk at the prompt, keeps the BIOS on C:.
// Keen goes in C:\ itself: it runs as the shell, which starts in the root.
const std::map<std::string, std::vector<Step>> kPresets = {
    {"keen1", {
        {"C:\\>", "@DISK1 a:\n"},
        {"A:\\>", "install\n"},
        {"Which drive to install to?", "c"},
        {"Enter directory name to copy into:", "@BS @BS @BS @BS @ENTER"},
        {"Authentic files Verified!", "@DONE"},
    }},
};

}  // namespace

int main(int argc, char **argv) {
    if (argc < 7 || !kPresets.count(argv[1])) {
        std::fprintf(stderr, "usage: %s <keen1> <bios> <egabios> <base.img> <out.img> <disk1.img>\n", argv[0]);
        return 2;
    }
    const std::vector<Step> &steps = kPresets.at(argv[1]);
    const char *budget_env = std::getenv("PC_AT_BUDGET");
    const uint64_t budget = budget_env ? std::strtoull(budget_env, nullptr, 10) : 30'000'000'000ull;
    const bool trace = std::getenv("PC_AT_TRACE") != nullptr;

    auto bios = ReadFile(argv[2]);
    auto vbios = ReadFile(argv[3]);
    auto hdd = ReadFile(argv[4]);
    const char *out_path = argv[5];
    auto disk1 = ReadFile(argv[6]);
    if (bios.empty() || vbios.empty() || hdd.empty() || disk1.empty()) {
        std::fprintf(stderr, "cannot open the ROMs, the base C: or the install floppy\n");
        return 2;
    }

    Machine m;
    m.reset();
    m.chipset.load_rom(0x100000 - bios.size(), bios.data(), bios.size());
    m.chipset.load_rom(0xC0000, vbios.data(), vbios.size());
    m.chipset.hdd.mount(0, hdd.data(), hdd.size());

    std::string last_screen, prev_screen, sent_screen;
    std::size_t step_idx = 0;
    bool done = false;
    uint64_t still_since = 0;
    const uint64_t kSettle = 2 * kHz;  // also covers DOS's disk-swap timing check

    const uint64_t kChunk = 100'000;
    for (uint64_t used = 0; used < budget && !done; used += kChunk) {
        m.run_cycles(int64_t(kChunk));
        std::string s = ScreenText(m);
        last_screen = s;
        if (s != prev_screen) {
            still_since = m.total_cycles();
            if (trace) std::fprintf(stderr, "\n===== screen @ %llu cycles =====\n%s",
                                    (unsigned long long)m.total_cycles(), s.c_str());
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
            else if (tok == "@BS") SendKey(m, kScanBackspace);
            else if (tok == "@DISK1") m.chipset.fdc.mount(0, disk1.data(), disk1.size());
            else {
                for (char c : tok) {
                    auto [mk, shift] = Set1MakeCode(c);
                    if (mk) SendKey(m, mk, shift);
                }
            }
        }
        sent_screen = s;
        ++step_idx;
    }

    if (!done) {
        std::fprintf(stderr,
            "FAILED: cycle budget (%llu) exhausted at step %zu of %zu (\"%s\"). Last screen:\n%s\n",
            (unsigned long long)budget, step_idx + 1, steps.size(),
            steps[step_idx].wait_for.c_str(), last_screen.c_str());
        return 1;
    }

    const std::vector<uint8_t> &final_image = m.chipset.hdd.image(0);
    std::ofstream out(out_path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(final_image.data()), std::streamsize(final_image.size()));
    if (!out) { std::fprintf(stderr, "failed to write %s\n", out_path); return 2; }
    std::fprintf(stderr, "wrote %s at cycle %llu\n", out_path, (unsigned long long)m.total_cycles());
    return 0;
}
