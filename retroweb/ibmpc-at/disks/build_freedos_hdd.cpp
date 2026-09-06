// Builds this machine's shipped hard disk image by actually running the
// real, unmodified FreeDOS 1.3 installer against this emulator end to end
// -- not by hand-crafting a FAT filesystem. Drives the installer's Y/N
// prompts, its FDISK/format/reboot sequence, and its "insert diskette
// containing file A:\FREEDOS.NNN" file-by-file requests (across all six
// install floppies) via injected keystrokes and floppy swaps, exactly as
// a person installing FreeDOS onto a real 5170-339 would, then saves the
// controller's own final disk image. See IBM_PCAT_REVIEW.md §11 for the
// full account of what this took to get right (several genuine emulator
// bugs were found and fixed via this exact process) and why this file
// looks the way it does.
//
// Usage:
//   build_freedos_hdd <bios> <vgabios> <floppy-dir> <out.img> [max_cycles]
//
// <floppy-dir> must contain the real FreeDOS 1.3 FD13-FloppyEdition "120m"
// set: x86BOOT.img, x86DSK01.img .. x86DSK06.img (see fetch-freedos-
// install-set.sh). <out.img> receives the finished 733/5/17,
// 31,900,160-byte raw HDD image once the installer reports completion (or
// the cycle budget runs out first, in which case this prints a clear
// failure and exits non-zero -- callers should not silently accept a
// partial image). [max_cycles] defaults to a generous budget verified
// sufficient to reach "installation... has completed" from a cold boot
// (see IBM_PCAT_REVIEW.md); running the real installer this way is
// legitimately slow, on the order of tens of billions of emulated 8MHz
// cycles, since every floppy read is paced to its real transfer rate --
// this is a build-time asset-generation tool, not the shipped emulator
// itself, so unlike everything under CLAUDE.md's "never speed these up"
// rule, taking real minutes here is expected and fine.

#include "../machine.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace ibmpcat;

namespace {

std::vector<uint8_t> ReadFile(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Reconstructs the 80x25 text-mode screen as plain ASCII, following the
// CRTC's current start-address register (so this stays correct even if
// the BIOS/DOS scrolls by moving the start offset instead of the bytes).
std::string ScreenText(Machine &m) {
    uint32_t base = 0x18000 + uint32_t(m.chipset.ega.start_offset()) * 2;
    std::string out;
    for (int row = 0; row < 25; ++row) {
        std::string line;
        for (int col = 0; col < 80; ++col) {
            uint32_t off = base + uint32_t(row * 80 + col) * 2;
            uint8_t ch = m.chipset.ega.vram[off & 0x3FFFF];
            line.push_back((ch >= 32 && ch < 127) ? char(ch) : ' ');
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line;
        out += '\n';
    }
    return out;
}

// Set 1 (XT) keyboard scancodes -- the well-documented, standard make-code
// table; break code = make code | 0x80. `inject_scancode()` delivers
// exactly what's passed, matching how a real keyboard's scan codes reach
// the 8042 (this is the same injection path the front end will eventually
// use for actual keystrokes).
uint8_t Set1MakeCode(char c) {
    static const std::map<char, uint8_t> table = {
        {'1', 0x02}, {'2', 0x03}, {'3', 0x04}, {'4', 0x05}, {'5', 0x06},
        {'6', 0x07}, {'7', 0x08}, {'8', 0x09}, {'9', 0x0A}, {'0', 0x0B},
        {'y', 0x15}, {'n', 0x31},
        {' ', 0x39}, {'\n', 0x1C}, {'\r', 0x1C},
    };
    auto it = table.find(c);
    return it != table.end() ? it->second : 0;
}

void SendKey(Machine &m, uint8_t make) {
    m.chipset.kbc.inject_scancode(make);
    m.run_cycles(20000);
    m.chipset.kbc.inject_scancode(uint8_t(make | 0x80));
    m.run_cycles(20000);
}
void SendString(Machine &m, const std::string &s) {
    for (char c : s) {
        uint8_t mk = Set1MakeCode(c);
        if (mk) SendKey(m, mk);
    }
}

struct Step {
    std::string wait_for;  // substring to watch for on screen
    std::string send;      // literal keys to type, or a "@..." action token
};

}  // namespace

int main(int argc, char **argv) {
    if (argc < 5) {
        std::fprintf(stderr,
            "usage: %s <bios> <vgabios> <floppy-dir> <out.img> [max_cycles]\n", argv[0]);
        return 2;
    }
    auto bios = ReadFile(argv[1]);
    auto vga = ReadFile(argv[2]);
    std::string floppy_dir = argv[3];
    std::string out_path = argv[4];
    uint64_t budget = argc > 5 ? std::strtoull(argv[5], nullptr, 10) : 60'000'000'000ull;

    if (bios.empty() || vga.empty()) {
        std::fprintf(stderr, "cannot open BIOS or VGABIOS image\n");
        return 2;
    }
    auto boot = ReadFile(floppy_dir + "/x86BOOT.img");
    if (boot.empty()) {
        std::fprintf(stderr, "cannot open %s/x86BOOT.img\n", floppy_dir.c_str());
        return 2;
    }

    Machine m;
    m.reset();
    m.chipset.load_rom(0x100000 - bios.size(), bios.data(), bios.size());
    m.chipset.load_rom(0xC0000, vga.data(), vga.size());
    m.chipset.fdc.mount(0, boot.data(), boot.size());

    // A genuinely blank, factory-fresh fixed disk -- no partition table,
    // no filesystem. Everything from here on is what the real installer
    // itself writes.
    std::vector<uint8_t> blank_hdd(733UL * 5 * 17 * 512, 0);
    m.chipset.hdd.mount(0, blank_hdd.data(), blank_hdd.size());

    // Each of these must outlive the run (Fdc765::mount() copies the
    // bytes in, but keeping the source buffers alive is simplest and
    // costs little -- six floppies' worth of memory).
    static std::vector<std::vector<uint8_t>> disk_images;
    std::string current_disk_image = "x86BOOT.img";
    auto swap_floppy = [&](const std::string &name) {
        disk_images.push_back(ReadFile(floppy_dir + "/" + name));
        if (disk_images.back().empty()) {
            std::fprintf(stderr, "cannot open %s/%s\n", floppy_dir.c_str(), name.c_str());
            std::exit(2);
        }
        m.chipset.fdc.mount(0, disk_images.back().data(), disk_images.back().size());
        current_disk_image = name;
    };

    // The install flow up through the first disk swap follows a fixed
    // sequence of prompts. From "Insert diskette #2" onward, the real
    // FreeDOS 1.3 installer instead names files individually ("Insert
    // diskette containing file A:\FREEDOS.NNN"), handled dynamically
    // below rather than as fixed steps, since which physical disk a
    // given file needs is arithmetic (see disk_for_file_num), not a
    // fixed sequence position.
    std::vector<Step> steps = {
        {"Do you want to proceed", "y\n"},
        {"Do you want to reboot now", "y\n"},
        {"Do you want to proceed", "y\n"},
        {"Do you want to format your drive", "y\n"},
        {"ready to install FreeDOS", "y\n"},
        {"Insert diskette #2", "@x86DSK01.img"},
        {"Press a key to continue", "@NAG"},
    };

    // The real split-archive layout, read directly out of each floppy's
    // own FAT12 root directory rather than guessed: DSK01=FREEDOS.001-019,
    // DSK02=.020-039, DSK03=.040-059, DSK04=.060-079, DSK05=.080-099,
    // DSK06=.100-114 -- clean, contiguous ranges, confirming the
    // installer processes files strictly in numeric order and only asks
    // for a new diskette exactly at a range boundary.
    auto disk_for_file_num = [](int n) -> const char * {
        if (n <= 19) return "x86DSK01.img";
        if (n <= 39) return "x86DSK02.img";
        if (n <= 59) return "x86DSK03.img";
        if (n <= 79) return "x86DSK04.img";
        if (n <= 99) return "x86DSK05.img";
        return "x86DSK06.img";
    };
    std::string last_handled_request;
    bool final_reboot_done = false;

    // Real DOS floppy-swap prompts commonly flush/discard whatever's
    // already in the keyboard buffer right before they actually start
    // waiting (a defensive measure against a stray keystroke satisfying
    // the wrong prompt) -- a burst of presses sent all at once, right
    // when the prompt's text first appears, can land entirely within
    // that flush window and never be seen. Matching what an actual
    // impatient user does instead -- pressing the key again every so
    // often until something happens -- is far more robust than giving
    // up after one burst.
    bool nagging = false;
    uint64_t nag_next_at = 0;
    int nag_presses_left = 0;

    std::string last_screen, prev_screen;
    std::size_t step_idx = 0;
    const uint64_t kChunk = 200'000;
    for (uint64_t used = 0; used < budget; used += kChunk) {
        m.run_cycles(kChunk);
        std::string s = ScreenText(m);
        last_screen = s;

        // Edge-triggered, not level-triggered: a prompt's text can linger
        // in on-screen scrollback long after it was actually answered
        // (everything printed before a reboot stays visible on the same
        // 25-line screen alongside the post-reboot reprint of the same
        // prompt) -- so only match the transition from "not present" to
        // "present".
        bool has_now = step_idx < steps.size() && s.find(steps[step_idx].wait_for) != std::string::npos;
        bool had_before = step_idx < steps.size() && prev_screen.find(steps[step_idx].wait_for) != std::string::npos;
        prev_screen = s;
        if (has_now && !had_before) {
            const std::string &send = steps[step_idx].send;
            nagging = false;  // any new match is real progress -- the NAG branch re-arms it if relevant
            if (!send.empty() && send[0] == '@') {
                if (send.substr(1) == "NAG") {
                    nagging = true;
                    nag_presses_left = 40;            // spread over a much longer window than one burst
                    nag_next_at = m.total_cycles();   // fire the first one immediately, then keep going
                } else {
                    swap_floppy(send.substr(1));
                }
            } else {
                SendString(m, send);
            }
            ++step_idx;
        }
        if (nagging && nag_presses_left > 0 && m.total_cycles() >= nag_next_at) {
            SendKey(m, Set1MakeCode(' '));
            nag_next_at = m.total_cycles() + 2'000'000;  // ~0.25s of emulated time between presses
            if (--nag_presses_left == 0) nagging = false;
        }

        // Dynamic per-file disk-swap handling, independent of the fixed
        // Step list above. Edge-triggered on the *value* of the requested
        // file number (via last_handled_request) rather than mere text
        // presence, since the number itself changes with each request.
        {
            const std::string marker = "A:\\FREEDOS.";
            // rfind, not find: the screen's scrollback can hold several
            // stacked "Insert diskette ..." lines from earlier, already-
            // handled requests (rotating marketing tips print between
            // nags) -- the most recent, currently-active request is
            // whichever one is lowest/last on screen.
            auto pos = s.rfind(marker);
            if (pos != std::string::npos && pos + marker.size() + 3 <= s.size()) {
                std::string request = s.substr(pos, marker.size() + 3);
                if (request != last_handled_request) {
                    int num = std::atoi(request.c_str() + marker.size());
                    const char *needed = disk_for_file_num(num);
                    if (needed != current_disk_image) swap_floppy(needed);
                    nagging = true;
                    nag_presses_left = 40;
                    nag_next_at = m.total_cycles();
                    last_handled_request = request;
                }
            }
        }

        // Final "installation has completed, reboot now?" prompt, also
        // independent of the fixed Step list since it comes after an
        // unknown number of dynamic per-file requests. This text is
        // unique to that one moment and final_reboot_done makes this
        // naturally single-fire, so a level check is sufficient here.
        if (!final_reboot_done &&
                s.find("installation of FreeDOS 1.3 has completed") != std::string::npos &&
                s.find("Do you want to reboot now") != std::string::npos) {
            SendString(m, "y\n");
            final_reboot_done = true;
            std::fprintf(stderr, "install reported complete at cycle %llu\n",
                         (unsigned long long)m.total_cycles());
            break;
        }
    }

    if (!final_reboot_done) {
        std::fprintf(stderr,
            "FAILED: cycle budget (%llu) exhausted before the installer reported completion.\n"
            "Last screen:\n%s\n", (unsigned long long)budget, last_screen.c_str());
        return 1;
    }

    // NOT the buffer passed to hdd.mount() above -- that only reflects
    // the *starting* (blank) state. Every byte the install actually wrote
    // landed in the controller's own internal image, which is where the
    // real final disk contents live (matching real hardware: the drive
    // owns its storage; mount() is just how it was loaded once).
    const auto &final_image = m.chipset.hdd.drives[0].image;
    std::ofstream out(out_path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(final_image.data()), std::streamsize(final_image.size()));
    if (!out) {
        std::fprintf(stderr, "failed to write %s\n", out_path.c_str());
        return 2;
    }
    std::fprintf(stderr, "wrote %s (%zu bytes)\n", out_path.c_str(), final_image.size());
    return 0;
}
