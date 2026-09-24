// Builds this machine's shipped hard disk image by actually running the
// real, unmodified FreeDOS 1.3 installer against this emulator end to end
// -- not by hand-crafting a FAT filesystem. Boots FreeDOS's own companion
// boot floppy (FD13BOOT.img) with the real FreeDOS 1.3 LiveCD ISO mounted
// on the ATAPI CD-ROM, and drives the installer's full-screen dialogs via
// injected keystrokes and screen-text matching, exactly as a person
// installing FreeDOS onto a real 486 would, then saves the controller's
// own final in-memory disk image. See PC486_REVIEW.md §5 for the account
// of what this took and why this file looks the way it does.
//
// Boot path: floppy, not El Torito. See PC486_REVIEW.md §5.1 -- this ISO's
// El Torito image is ISOLINUX + MEMDISK, which needs protected mode this
// Milestone 1 real-mode-only core does not have. The floppy is what
// FreeDOS ships FD13BOOT.img for.
//
// Usage:
//   build_freedos_hdd <bios> <vgabios> <boot.img> <cd.iso> <out.img> [max_cycles]
//
// <out.img> receives the finished 1024/16/63, 528,482,304-byte raw HDD
// image once the installer reports completion (or the cycle budget runs
// out first, in which case this prints a clear failure and exits non-zero
// -- callers must not silently accept a partial image). Running the real
// installer this way is legitimately slow, on the order of tens of
// billions of emulated 66MHz cycles, since every floppy/CD read is paced
// to its real transfer rate -- this is a build-time asset-generation tool,
// not the shipped emulator itself, so unlike everything under CLAUDE.md's
// "never speed these up" rule, taking real minutes here is expected and
// fine.
//
// PC486_TRACE=1 in the environment dumps every screen change to stderr,
// which is how the step sequence below was discovered in the first place.

#include "../machine.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// Reconstructs the 80x25 text-mode screen as plain ASCII, following the
// CRTC's current start-address register (so this stays correct even if the
// BIOS/DOS scrolls by moving the start offset instead of the bytes).
// Follows the real planar VRAM layout ega.h documents and ega_render.cpp
// uses -- vram[(plane_offset << 2) + plane], plane 0 = character.
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

// Set 1 (XT) keyboard scancodes -- the standard make-code table; break
// code = make code | 0x80. inject_scancode() delivers exactly what's
// passed, matching how a real keyboard's scan codes reach the 8042.
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

// Extended (grey) keys -- arrows and friends -- arrive as an 0xE0 prefix
// byte followed by the base make code, and the break as 0xE0 + (code|0x80).
// This is how a real 101-key keyboard reports them in scan code set 1.
// Every byte needs its own gap, not just each make/break pair: the 8042 has
// one output register, and a byte queued behind an unread one is not
// announced again until the guest's handler drains the first -- so two bytes
// sent in the same instant wedge the controller with an undelivered byte. A
// real keyboard cannot outrun that (it clocks one bit at a time), and the
// browser front end spaces every byte of a multi-byte sequence for the same
// reason (web/app.js's injectScancodeSequence).
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

// One dialog rule. Matching is edge-triggered on `wait_for` appearing on
// screen, and NOT position-ordered: the installer genuinely runs its early
// stages twice (it reboots itself after partitioning, then comes back
// through the language/welcome screens with drive C: now partitioned), so
// `uses` says how many times a given prompt is expected rather than
// pinning it to one slot in a fixed sequence.
// Selects FreeDOS's own boot-menu entry 4 as the installed system's default,
// by changing the single digit in the FDCONFIG.SYS the installer just wrote.
//
// Why this is needed, and why it is a one-digit edit rather than a different
// answer to the installer: FreeDOS's installer picks its CONFIG.SYS template
// purely from CPU/environment detection (`fdins900.bat` runs `vinfo /m`, sets
// FEXT=486 for this machine, finds no CONFIG.486 on the media, and falls back
// to CONFIG.DEF), asks the user nothing about memory management anywhere in
// its FDASK sequence, and CONFIG.DEF hardcodes `MENUDEFAULT=2,5` instead of
// using the $FDEFMENU$ placeholder its other templates use. So there is no
// installer answer that produces a JEMM-free config -- it cannot be steered.
//
// Menu entry 2 loads JEMMEX, which is a V86-mode monitor (its own readme: EMS
// via "VCPI services to allow DOS applications running in V86-mode", and a
// whole section on "Emulation of privileged Opcodes"). This Milestone 1 core
// has no protected mode and no V86 at all, so JEMMEX sets up its monitor,
// writes CR0, and jumps into what it believes is V86 mode -- landing in the
// interrupt vector table and spinning there forever. Entry 4 ("Load FreeDOS
// low with some drivers (Safe Mode)") loads only HIMEMX, which genuinely
// works on this machine. All five of FreeDOS's entries stay present and
// selectable; only which one the 5-second timeout picks changes.
//
// This is a labelled departure per CLAUDE.md, and it should be reverted once
// a later milestone implements protected mode + V86. See PC486_REVIEW.md §5.9.
//
// The search is anchored on the whole MENUDEFAULT+MENU-1 block rather than the
// bare "MENUDEFAULT=" string, because a full FreeDOS install contains ten
// copies of the latter in package documentation and only one of the former.
bool SelectRealModeBootMenuEntry(std::vector<uint8_t> &image) {
    static const std::string kNeedle =
        "MENUDEFAULT=2,5\r\nMENU 1 - Load FreeDOS with JEMMEX";
    const auto it = std::search(image.begin(), image.end(), kNeedle.begin(), kNeedle.end());
    if (it == image.end()) {
        std::fprintf(stderr, "FAILED: installed FDCONFIG.SYS boot-menu block not found; "
                             "the installer's CONFIG.DEF template may have changed\n");
        return false;
    }
    // Exactly one match, or this is patching something other than FDCONFIG.SYS.
    if (std::search(it + 1, image.end(), kNeedle.begin(), kNeedle.end()) != image.end()) {
        std::fprintf(stderr, "FAILED: boot-menu block found more than once; refusing to patch\n");
        return false;
    }
    const std::size_t digit = std::size_t(it - image.begin()) + std::strlen("MENUDEFAULT=");
    if (image[digit] != '2') {
        std::fprintf(stderr, "FAILED: expected '2' at MENUDEFAULT, found '%c'\n", image[digit]);
        return false;
    }
    image[digit] = '4';
    std::fprintf(stderr, "set FDCONFIG.SYS MENUDEFAULT to entry 4 (HIMEMX, no JEMMEX) "
                         "at image offset %zu -- see PC486_REVIEW.md §5.9\n", digit);
    return true;
}

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
        // Scoped so the 400MB source buffer is released as soon as the
        // device has its own copy -- this tool already holds two
        // half-gigabyte HDD buffers.
        auto iso = ReadFile(iso_path);
        if (iso.empty()) { std::fprintf(stderr, "cannot open %s\n", iso_path.c_str()); return 2; }
        m.chipset.cdrom.mount(iso.data(), iso.size());
    }

    // A genuinely blank, factory-fresh fixed disk -- no partition table, no
    // filesystem. 1024 cyl / 16 head / 63 sec, the pre-EIDE CHS ceiling
    // Wd1003::mount() and configure_factory_cmos() both describe. Everything
    // from here on is what the real installer itself writes.
    {
        std::vector<uint8_t> blank_hdd(1024ULL * 16 * 63 * 512, 0);
        m.chipset.hdd.mount(0, blank_hdd.data(), blank_hdd.size());
    }

    // The FreeDOS 1.3 installer's dialog sequence, as observed by actually
    // running it (PC486_TRACE=1) rather than assumed -- see
    // PC486_REVIEW.md §5.2. Unlike ibmpc-at's FloppyEdition flow there is
    // no multi-disk swapping at all: FDAUTO.BAT loads the CD driver
    // (UDVD2.SYS) + SHSUCDX, then SETUP.BAT hands off to the copy on the
    // CD, which is the sole source for every package.
    // Every dialog is a V8Power `vchoice` option box: arrow keys move the
    // highlight, Enter accepts it, and the batch file's own `/d N` switch
    // says which entry starts highlighted. That switch is why the three
    // destructive prompts below need "@UP @ENTER" rather than a bare Enter
    // -- stage400/stage500/stage800 all pass `/d 2`, i.e. they deliberately
    // start on "No - Return to DOS", so answering with Enter alone aborts
    // the install (observed: "The installation of FreeDOS 1.3 has been
    // aborted"). Defaults read directly out of the installer's own
    // stage*.bat / fdask*.bat on the CD, not guessed.
    std::vector<Step> steps = {
        // stage300: language list (`/p`, English already highlighted) and the
        // welcome/proceed box (no `/d`, so "Yes - Continue" is default).
        // Both appear twice -- once per pass over the installer.
        {"What is your preferred language?", "@ENTER", 2},
        {"Do you want to proceed?",          "@ENTER", 2},
        // stage400: partition drive C:. `/d 2` -> starts on No.
        {"Do you want to partition your drive?", "@UP @ENTER", 1},
        // stage400: the reboot that makes the new partition table take
        // effect (no `/d`, so "Yes - Please reboot now" is default). This is
        // a genuine machine reboot back through the same boot floppy.
        {"scheme to take effect",            "@ENTER", 1},
        // stage500: format drive C:. `/d 2` -> starts on No.
        {"Do you want to format your drive?", "@UP @ENTER", 1},
        // stage700's FDASK000-FDASK700 questions (keyboard layout, target
        // directory, config-file handling, package set, ...). Every one of
        // them passes `/d 1` or a preselect, i.e. the highlighted entry is
        // already the wanted answer, so each just needs Enter.
        {"Please select your keyboard layout", "@ENTER", 4},
        {"packages do you want to install",    "@ENTER", 4},
        {"Change installation target directory", "@ENTER", 4},
        {"Replace the system configuration files", "@ENTER", 4},
        {"Transfer system files to drive",     "@ENTER", 4},
        {"Force new boot sector code on drive", "@ENTER", 4},
        {"backup the old files before installing", "@ENTER", 4},
        {"Remove all old files from",          "@ENTER", 4},
        // stage800: the final go/no-go. `/d 2` -> starts on No.
        {"Do you want to install now?",        "@UP @ENTER", 1},
        // stage900: the installer's own completion message. Reaching this is
        // the whole point -- the image is saved, and the reboot it offers is
        // deliberately NOT taken (nothing more needs to be written).
        {"is now complete",                    "@DONE", 1},
    };

    // A genuinely unrecognized opcode is the first thing worth knowing if a
    // future run stops making progress, so this hook stays wired up rather
    // than being scaffolding that gets removed.
    std::map<uint32_t, uint64_t> unimpl;
    m.cpu.on_unimplemented = [&](uint16_t cs, uint32_t ip, uint16_t op) {
        uint32_t key = (uint32_t(cs) << 16) | uint16_t(ip);
        if (unimpl[key]++ == 0)
            std::fprintf(stderr, "UNIMPLEMENTED opcode %04X at %04X:%04X\n", op, cs, uint16_t(ip));
    };

    std::string last_screen, prev_screen;
    std::size_t step_idx = 0;
    bool done = false;
    // The rule currently being answered, how long the screen has been still,
    // and when a re-press is allowed. kSettle is how quiet the screen must go
    // before a key is sent; kRetryGap is how long to wait before concluding
    // the key never landed. Both are in 66MHz cycles (~1.2s and ~3s of
    // emulated time) -- generous, because this tool's wall-clock cost is
    // dominated by the install itself, not by these waits.
    Step *armed = nullptr;
    uint64_t sent_at = 0;
    int retries = 0;
    uint64_t still_since = 0;
    const uint64_t kSettle = 80'000'000;    // ~1.2s of emulated time before the
                                            // first press, so it can't land in
                                            // the dialog's keyboard-flush window
    const uint64_t kRetryGap = 200'000'000; // ~3s between nag presses
    const int kMaxRetries = 30;             // bounded, so a genuinely stuck
                                            // prompt fails the run rather than
                                            // hammering keys forever

    const uint64_t kChunk = 500'000;
    for (uint64_t used = 0; used < budget; used += kChunk) {
        m.run_cycles(int64_t(kChunk));
        std::string s = ScreenText(m);
        last_screen = s;

        if (trace && s != prev_screen) {
            std::fprintf(stderr, "\n===== screen @ %llu cycles =====\n%s",
                         (unsigned long long)m.total_cycles(), s.c_str());
        }

        // Edge-triggered, not level-triggered: a prompt's text can linger in
        // on-screen scrollback long after it was answered (everything printed
        // before a reboot stays visible on the same 25-line screen alongside
        // the post-reboot reprint of the same prompt).
        // Track how long the screen has been still. A dialog's text appears
        // well before `vchoice` actually starts reading the keyboard -- the
        // batch file is still drawing the frame and the option box -- and a
        // key pressed inside that window is simply discarded (the same
        // keyboard-flush race ibmpc-at hit, IBM_PCAT_REVIEW.md §11). Waiting
        // for the screen to go quiet is what a real person does anyway.
        if (s != prev_screen) still_since = m.total_cycles();

        for (auto &st : steps) {
            // `uses` gates taking a *new* match, not re-pressing one already
            // in progress -- otherwise a rule's final use consumes its budget
            // and the nag below can never fire for it.
            if (st.uses <= 0 && armed != &st) continue;
            if (s.find(st.wait_for) == std::string::npos) continue;
            if (prev_screen.find(st.wait_for) != std::string::npos && armed != &st) continue;
            // Matched: arm it, then hold off until the screen settles.
            if (armed != &st) { armed = &st; retries = 0; sent_at = 0; }
            if (m.total_cycles() - still_since < kSettle) break;
            // Keep pressing on a cadence for as long as the prompt is still
            // on screen, rather than pressing once and hoping. This is the
            // same "nag" mechanism ibmpc-at needed (IBM_PCAT_REVIEW.md §11)
            // and it is genuinely required here too: stage300 calls `vchoice`
            // from inside a redraw loop, so a single Enter is routinely
            // consumed by the wrong iteration and the installer sits there.
            // Re-pressing is safe because each rule's key sequence is
            // idempotent against a `vchoice` option box -- Up clamps at the
            // top entry instead of wrapping, so an extra "Up Enter" keeps
            // selecting the same "Yes" rather than walking onto "No"
            // (verified: all three destructive prompts took 3-4 presses in a
            // full run and every one of them still partitioned/formatted/
            // installed instead of aborting).
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
            // A rule's `send` is a space-separated list of key tokens, so a
            // dialog needing "move the highlight, then accept" is one rule.
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

    // The Bochs-legacy BIOS writes its own BX_INFO/BX_PANIC progress and
    // failure text to port 0xE9 (the Bochs debug-console convention), which
    // the chipset captures -- the most direct evidence available about what
    // the firmware itself thinks is happening.
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

    // NOT the buffer passed to hdd.mount() above -- that only reflects the
    // *starting* (blank) state. Every byte the install actually wrote landed
    // in the controller's own internal image, which is where the real final
    // disk contents live (matching real hardware: the drive owns its
    // storage; mount() is just how it was loaded once).
    std::vector<uint8_t> final_image = m.chipset.hdd.image(0);
    if (!SelectRealModeBootMenuEntry(final_image)) return 1;
    std::ofstream out(out_path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(final_image.data()), std::streamsize(final_image.size()));
    if (!out) { std::fprintf(stderr, "failed to write %s\n", out_path.c_str()); return 2; }
    std::fprintf(stderr, "wrote %s (%zu bytes) at cycle %llu\n",
                 out_path.c_str(), final_image.size(), (unsigned long long)m.total_cycles());
    return 0;
}
