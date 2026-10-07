// Builds the shipped HDD image by running the real FreeDOS 1.3 installer
// against this emulator, driving its prompts with injected keystrokes and
// floppy swaps across all six install floppies (IBM_PCAT_REVIEW.md §11).
//
// Usage:
//   build_freedos_hdd <bios> <vgabios> <floppy-dir> <out.img> [max_cycles]
//
// <floppy-dir> needs the FD13-FloppyEdition "120m" set: x86BOOT.img and
// x86DSK01.img .. x86DSK06.img (fetch-freedos-install-set.sh). <out.img>
// receives the 733/5/17, 31,900,160-byte raw image. If max_cycles runs out
// first, it prints a failure and exits non-zero. A run takes tens of
// billions of emulated 8MHz cycles because floppy reads are paced realistically.

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

// Reads the 80x25 text screen from the CRTC start address, using the planar
// VRAM layout from ega.h: vram[(plane_offset << 2) + plane], plane 0 = character.
std::string ScreenText(Machine &m) {
    const auto &ega = m.chipset.ega;
    std::string out;
    for (int row = 0; row < 25; ++row) {
        std::string line;
        for (int col = 0; col < 80; ++col) {
            uint32_t plane_off = (uint32_t(ega.start_offset()) + uint32_t(row * 80 + col)) & 0xFFFF;
            uint8_t ch = ega.vram[(plane_off << 2) + 0];
            line.push_back((ch >= 32 && ch < 127) ? char(ch) : ' ');
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line;
        out += '\n';
    }
    return out;
}

// Set 1 (XT) make codes; break code = make | 0x80.
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

    // Blank factory-fresh fixed disk; the installer writes everything else.
    std::vector<uint8_t> blank_hdd(733UL * 5 * 17 * 512, 0);
    m.chipset.hdd.mount(0, blank_hdd.data(), blank_hdd.size());

    // Source buffers stay alive for the run.
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

    // Fixed prompt sequence up to the first swap. After "Insert diskette #2"
    // the installer names files individually and swaps are computed below.
    std::vector<Step> steps = {
        {"Do you want to proceed", "y\n"},
        {"Do you want to reboot now", "y\n"},
        {"Do you want to proceed", "y\n"},
        {"Do you want to format your drive", "y\n"},
        {"ready to install FreeDOS", "y\n"},
        {"Insert diskette #2", "@x86DSK01.img"},
        {"Press a key to continue", "@NAG"},
    };

    // Split-archive layout from each floppy's FAT12 root: DSK01=FREEDOS.001-019,
    // DSK02=020-039, DSK03=040-059, DSK04=060-079, DSK05=080-099, DSK06=100-114.
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

    // DOS swap prompts flush the keyboard buffer as they start, so a single
    // burst can be lost. Re-press the key periodically until the prompt moves on.
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

        // Edge-triggered: an answered prompt can linger in scrollback, so match only
        // the transition from absent to present.
        bool has_now = step_idx < steps.size() && s.find(steps[step_idx].wait_for) != std::string::npos;
        bool had_before = step_idx < steps.size() && prev_screen.find(steps[step_idx].wait_for) != std::string::npos;
        prev_screen = s;
        if (has_now && !had_before) {
            const std::string &send = steps[step_idx].send;
            nagging = false;  // new match is progress; NAG re-arms it
            if (!send.empty() && send[0] == '@') {
                if (send.substr(1) == "NAG") {
                    nagging = true;
                    nag_presses_left = 40;            // spread over a longer window than one burst
                    nag_next_at = m.total_cycles();   // first press fires immediately
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

        // Per-file disk swaps, edge-triggered on the requested file number.
        {
            const std::string marker = "A:\\FREEDOS.";
            // rfind: scrollback can hold older "Insert diskette" lines; the active request is the last.
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

        // Final completion prompt. Its text is unique and final_reboot_done makes it single-fire.
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

    // The drive's own image holds the installed contents; the buffer passed to mount() is the blank start.
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
