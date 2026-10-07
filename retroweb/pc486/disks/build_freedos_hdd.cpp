// Builds the shipped HDD image by running the real FreeDOS 1.3 installer on this
// emulator: boots the FD13BOOT.img floppy with the LiveCD ISO on the ATAPI CD-ROM
// and drives the installer's dialogs with injected keys and screen-text matching.
// See PC486_REVIEW.md §5.
// Boot path is floppy, not El Torito: the ISO's image is ISOLINUX + MEMDISK, which
// needs protected mode (PC486_REVIEW.md §5.1).
//
// Usage: build_freedos_hdd <bios> <vgabios> <boot.img> <cd.iso> <out.img> [max_cycles]
//
// <out.img> receives the finished 1010/9/55, 255,974,400-byte raw image. If the cycle
// budget runs out first it prints a failure and exits non-zero. Slow by design
// (tens of billions of cycles, floppy/CD reads paced at real rates); this is a
// build-time tool, not the shipped emulator.
// PC486_TRACE=1 dumps every screen change to stderr.

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

// 80x25 text screen as ASCII, following the CRTC start address so scrolling by
// start offset works. Planar VRAM per ega.h: vram[(plane_offset << 2) + plane], plane 0 = character.
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

// Set 1 (XT) make codes; break = make | 0x80
uint8_t Set1MakeCode(char c) {
    static const std::map<char, uint8_t> table = {
        {'1', 0x02}, {'2', 0x03}, {'3', 0x04}, {'4', 0x05}, {'5', 0x06},
        {'6', 0x07}, {'7', 0x08}, {'8', 0x09}, {'9', 0x0A}, {'0', 0x0B},
        {'q', 0x10}, {'w', 0x11}, {'e', 0x12}, {'r', 0x13}, {'t', 0x14},
        {'y', 0x15}, {'u', 0x16}, {'i', 0x17}, {'o', 0x18}, {'p', 0x19},
        {'a', 0x1E}, {'s', 0x1F}, {'d', 0x20}, {'f', 0x21}, {'g', 0x22},
        {'h', 0x23}, {'j', 0x24}, {'k', 0x25}, {'l', 0x26},
        {'z', 0x2C}, {'x', 0x2D}, {'c', 0x2E}, {'v', 0x2F}, {'b', 0x30},
        {'n', 0x31}, {'m', 0x32},
        {' ', 0x39}, {'\n', 0x1C}, {'\r', 0x1C}, {'\x1b', 0x01},
    };
    auto it = table.find(c);
    return it != table.end() ? it->second : 0;
}

void SendKey(Machine &m, uint8_t make) {
    m.chipset.kbc.inject_scancode(make);
    m.run_cycles(150000);
    m.chipset.kbc.inject_scancode(uint8_t(make | 0x80));
    m.run_cycles(150000);
}

// Extended keys are an 0xE0 prefix then the make code; break is 0xE0 + (code|0x80).
// Every byte needs its own gap: the 8042 has one output register, and a byte queued
// behind an unread one wedges it. The browser front end spaces them too
// (web/app.js injectScancodeSequence).
void SendExtendedKey(Machine &m, uint8_t make) {
    m.chipset.kbc.inject_scancode(0xE0);
    m.run_cycles(150000);
    m.chipset.kbc.inject_scancode(make);
    m.run_cycles(150000);
    m.chipset.kbc.inject_scancode(0xE0);
    m.run_cycles(150000);
    m.chipset.kbc.inject_scancode(uint8_t(make | 0x80));
    m.run_cycles(150000);
}

constexpr uint8_t kScanUp = 0x48, kScanDown = 0x50, kScanEnter = 0x1C, kScanEsc = 0x01;

void SendString(Machine &m, const std::string &s) {
    for (char c : s) {
        uint8_t mk = Set1MakeCode(c);
        if (mk) SendKey(m, mk);
    }
}

// One dialog rule, matched edge-triggered on `wait_for` appearing, not in order:
// the installer runs its early stages twice (it reboots after partitioning), so
// `uses` says how many times a prompt is expected.
struct Step {
    std::string wait_for;  // substring to watch for on screen
    std::string send;      // key action tokens ("@ENTER @UP"), or literal keys
    int uses = 1;          // how many times this prompt legitimately appears
};

}  // namespace

int main(int argc, char **argv) {
    if (argc < 6) {
        std::fprintf(stderr,
            "usage: %s <bios> <vgabios> <boot.img> <cd.iso> <out.img> [max_cycles]\n", argv[0]);
        return 2;
    }
    const std::string bios_path = argv[1], vga_path = argv[2];
    const std::string boot_path = argv[3], iso_path = argv[4], out_path = argv[5];
    uint64_t budget = argc > 6 ? std::strtoull(argv[6], nullptr, 10) : 400'000'000'000ull;
    const bool trace = std::getenv("PC486_TRACE") != nullptr;

    Machine m;
    m.reset();
    {
        auto bios = ReadFile(bios_path);
        auto vga = ReadFile(vga_path);
        if (bios.empty() || vga.empty()) {
            std::fprintf(stderr, "cannot open BIOS or VGABIOS image\n");
            return 2;
        }
        m.chipset.load_rom(0x100000 - uint32_t(bios.size()), bios.data(), bios.size());
        m.chipset.load_rom(0xC0000, vga.data(), vga.size());
    }
    {
        auto boot = ReadFile(boot_path);
        if (boot.empty()) { std::fprintf(stderr, "cannot open %s\n", boot_path.c_str()); return 2; }
        m.chipset.fdc.mount(0, boot.data(), boot.size());
    }
    {
        // Scoped so the 400MB source buffer is freed once the device has its copy
        auto iso = ReadFile(iso_path);
        if (iso.empty()) { std::fprintf(stderr, "cannot open %s\n", iso_path.c_str()); return 2; }
        m.chipset.cdrom.mount(iso.data(), iso.size());
    }

    // Blank factory-fresh disk: WD Caviar AC2250, 1010 cyl / 9 head / 55 sec. No
    // partition table or filesystem; the installer writes everything.
    {
        std::vector<uint8_t> blank_hdd(pc486::Wd1003::kImageBytes, 0);
        m.chipset.hdd.mount(0, blank_hdd.data(), blank_hdd.size());
    }

    // FreeDOS 1.3 installer dialog sequence as observed with PC486_TRACE=1
    // (PC486_REVIEW.md §5.2). No disk swapping: FDAUTO.BAT loads UDVD2.SYS + SHSUCDX
    // and SETUP.BAT hands off to the copy on the CD.
    // Every dialog is a V8Power `vchoice` box: arrows move, Enter accepts, `/d N` sets
    // the initial highlight. stage400/500/800 pass `/d 2` ("No - Return to DOS"), so
    // those prompts need "@UP @ENTER"; bare Enter aborts the install.
    std::vector<Step> steps = {
        // stage300: language list (English highlighted) and welcome box. Both appear twice.
        {"What is your preferred language?", "@ENTER", 2},
        {"Do you want to proceed?",          "@ENTER", 2},
        // stage400: partition drive C:
        {"Do you want to partition your drive?", "@UP @ENTER", 1},
        // stage400: reboot so the partition table takes effect, back through the boot floppy
        {"scheme to take effect",            "@ENTER", 1},
        // stage500: format drive C:
        {"Do you want to format your drive?", "@UP @ENTER", 1},
        // stage700 FDASK000-FDASK700 questions: the highlighted entry is already the
        // wanted answer, so Enter, except the package-set question below.
        {"Please select your keyboard layout", "@ENTER", 4},
        // The installer defaults to "Full installation including applications and
        // games" (option 3 of 4). This ships the Base set instead: 65 packages, no
        // games/apps/networking (FreeDOS 1.3 report, ibiblio distributions/1.3/official/
        // report.html), so two Ups select "Plain DOS system". BOOM/FreeDoom is a Games
        // package and absent from Base.
        {"packages do you want to install",    "@UP @UP @ENTER", 4},
        {"Change installation target directory", "@ENTER", 4},
        {"Replace the system configuration files", "@ENTER", 4},
        {"Transfer system files to drive",     "@ENTER", 4},
        {"Force new boot sector code on drive", "@ENTER", 4},
        {"backup the old files before installing", "@ENTER", 4},
        {"Remove all old files from",          "@ENTER", 4},
        // stage800: final go/no-go
        {"Do you want to install now?",        "@UP @ENTER", 1},
        // stage900: completion message. The image is saved here; the offered reboot is not taken.
        {"is now complete",                    "@DONE", 1},
    };

    // An unrecognized opcode is the first sign of a stuck run, so this hook stays wired up
    std::map<uint32_t, uint64_t> unimpl;
    m.cpu.on_unimplemented = [&](uint16_t cs, uint32_t ip, uint16_t op) {
        uint32_t key = (uint32_t(cs) << 16) | uint16_t(ip);
        if (unimpl[key]++ == 0)
            std::fprintf(stderr, "UNIMPLEMENTED opcode %04X at %04X:%04X\n", op, cs, uint16_t(ip));
    };

    std::string last_screen, prev_screen;
    std::size_t step_idx = 0;
    bool done = false;
    // Rule being answered, how long the screen has been still, and when a re-press
    // is allowed. Both waits are in 66MHz cycles, generous since the install dominates.
    Step *armed = nullptr;
    uint64_t sent_at = 0;
    int retries = 0;
    uint64_t still_since = 0;
    const uint64_t kSettle = 80'000'000;    // ~1.2s emulated, past the keyboard-flush window
    const uint64_t kRetryGap = 200'000'000; // ~3s between nag presses
    const int kMaxRetries = 30;             // a stuck prompt fails the run

    const uint64_t kChunk = 500'000;
    for (uint64_t used = 0; used < budget; used += kChunk) {
        m.run_cycles(int64_t(kChunk));
        std::string s = ScreenText(m);
        last_screen = s;

        if (trace && s != prev_screen) {
            std::fprintf(stderr, "\n===== screen @ %llu cycles =====\n%s",
                         (unsigned long long)m.total_cycles(), s.c_str());
        }

        // Edge-triggered: a prompt's text lingers in scrollback after it is answered,
        // and reprints after a reboot
        // Track how long the screen has been still. Dialog text appears before `vchoice`
        // reads the keyboard, and a key in that window is discarded (the ibmpc-at
        // keyboard-flush race, IBM_PCAT_REVIEW.md §11).
        if (s != prev_screen) still_since = m.total_cycles();

        for (auto &st : steps) {
            // `uses` gates taking a new match, not re-pressing one in progress
            if (st.uses <= 0 && armed != &st) continue;
            if (s.find(st.wait_for) == std::string::npos) continue;
            if (prev_screen.find(st.wait_for) != std::string::npos && armed != &st) continue;
            // Matched: arm it and wait for the screen to settle
            if (armed != &st) { armed = &st; retries = 0; sent_at = 0; }
            if (m.total_cycles() - still_since < kSettle) break;
            // Re-press on a cadence while the prompt is on screen (the ibmpc-at "nag",
            // IBM_PCAT_REVIEW.md §11): stage300 calls `vchoice` inside a redraw loop, so a
            // single Enter is often eaten. Safe because Up clamps at the top entry, so an
            // extra "Up Enter" keeps selecting "Yes" (all three destructive prompts took
            // 3-4 presses and still proceeded).
            if (sent_at != 0) {
                if (m.total_cycles() - sent_at < kRetryGap) break;
                if (retries >= kMaxRetries) break;
                ++retries;
            }
            if (sent_at == 0) --st.uses;
            ++step_idx;
            std::fprintf(stderr, "[cyc %12llu] \"%s\" -> %s%s\n",
                         (unsigned long long)m.total_cycles(), st.wait_for.c_str(), st.send.c_str(),
                         retries ? "  (nag -- prompt still on screen)" : "");
            if (st.send == "@DONE") { done = true; break; }
            // `send` is a space-separated token list, so "move, then accept" is one rule
            std::size_t pos = 0;
            while (pos < st.send.size()) {
                std::size_t sp = st.send.find(' ', pos);
                std::string tok = st.send.substr(pos, sp == std::string::npos ? sp : sp - pos);
                pos = (sp == std::string::npos) ? st.send.size() : sp + 1;
                if (tok.empty()) continue;
                if (tok == "@ENTER") SendKey(m, kScanEnter);
                else if (tok == "@ESC") SendKey(m, kScanEsc);
                else if (tok == "@UP") SendExtendedKey(m, kScanUp);
                else if (tok == "@DOWN") SendExtendedKey(m, kScanDown);
                else SendString(m, tok);
            }
            sent_at = m.total_cycles();
            break;  // one rule per observed screen change
        }
        if (armed && s.find(armed->wait_for) == std::string::npos) {
            armed = nullptr;  // prompt answered and gone
            sent_at = 0;
        }
        prev_screen = s;
        if (done) break;
    }

    // The Bochs-legacy BIOS writes BX_INFO/BX_PANIC text to port 0xE9 (debug-console
    // convention), which the chipset captures
    if (trace) {
        const std::string &dbg = m.chipset.debug_console();
        std::fprintf(stderr, "=== BIOS debug console (%zu bytes) ===\n%s\n=== end ===\n",
                     dbg.size(), dbg.size() > 4000 ? dbg.substr(dbg.size() - 4000).c_str() : dbg.c_str());
    }
    if (!done) {
        std::fprintf(stderr,
            "FAILED: cycle budget (%llu) exhausted before the installer reported completion.\n"
            "Reached step %zu of %zu. Last screen:\n%s\n",
            (unsigned long long)budget, step_idx, steps.size(), last_screen.c_str());
        return 1;
    }

    // Not the buffer passed to hdd.mount() (the blank starting state): installed bytes
    // land in the controller's own internal image
    std::vector<uint8_t> final_image = m.chipset.hdd.image(0);
    std::ofstream out(out_path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(final_image.data()), std::streamsize(final_image.size()));
    if (!out) { std::fprintf(stderr, "failed to write %s\n", out_path.c_str()); return 2; }
    std::fprintf(stderr, "wrote %s (%zu bytes) at cycle %llu\n",
                 out_path.c_str(), final_image.size(), (unsigned long long)m.total_cycles());
    return 0;
}
