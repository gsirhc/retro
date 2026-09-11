// Build-time tool: types one Example program into the board's own real
// resident editor (the same ROM every visitor gets), headlessly, once --
// then dumps the resulting source buffer to a small binary the browser
// pokes straight into RAM (Machine::pokeRam, wasm_machine.cpp) via
// app.js's pokeExample(). This exists to sidestep the still-not-fully-
// understood Examples-load wedging bug (CGOAC6502_REVIEW.md, "Examples-
// load wedging the CPU") by never running the real-visitor ACIA/NMI
// serial-reception simulation for Example programs at all. Also runs the
// board's real ASM over the typed source (build-time only, output
// discarded) purely as a fail-loud correctness check -- a broken example
// should never reach a visitor's browser. The result the browser actually
// gets is source-only: the visitor still types ASM themselves, watches it
// really compile, same as any hand-typed program. Run by web/Makefile's
// `examples-bin` target; never shipped as source, never run by a
// visitor's browser.
//
// Usage: gen_example_bin <firmware.bin> <firmware.lbl> <source.asm> <output.bin>
//
// Output format: [2] srcLen (little-endian) [srcLen] source buffer bytes
// (from SRC_START, up to and including the 2-byte 0,0 end-of-buffer
// sentinel).
//
// Appends "JMP $<SHELL_PROMPT>" as the program's real last line before
// assembling -- the same resume convention a live LOAD has always
// appended (see app.js's pokeExample()). This is about the *typed
// program itself* being well-formed, not about pre-assembling: whenever
// the visitor eventually RUNs it, it still needs somewhere sane to land
// on completion rather than falling off its own end into zero-filled RAM
// (a BRK/IRQ cascade -- see CGOAC6502_REVIEW.md for a case where that
// merely *looked* like a clean return, purely by stack-corruption luck).
// SHELL_PROMPT is read from firmware.lbl (gen_entrypoints.py's own
// technique) rather than hardcoded, since it's a linker-placed address
// that drifts as editor.s's source size changes.
#include "../machine.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

using namespace machine;

namespace {

// SRC_START is an editor.s compile-time constant (`=`, not a linker-
// placed label), so it doesn't appear in firmware.lbl -- hardcode it here
// the same way tests/assembler_test.cpp's kObjStart already does, citing
// editor.s as the source of truth. Stable across ROM rebuilds unless
// editor.s's own memory-split header changes.
constexpr uint16_t kShellEntry = 0x8000;
constexpr uint16_t kSrcStart = 0x3000;

// Reads one "al <hex> .<NAME>" line's address out of firmware.lbl --
// same format/technique as gen_entrypoints.py.
uint16_t readLabel(const std::string& lblPath, const std::string& name) {
    std::ifstream f(lblPath);
    if (!f) { std::fprintf(stderr, "%s: cannot open\n", lblPath.c_str()); std::exit(1); }
    std::regex re("^al ([0-9A-Fa-f]+) \\." + name + "\\s*$");
    std::string line;
    while (std::getline(f, line)) {
        std::smatch mo;
        if (std::regex_match(line, mo, re)) return uint16_t(std::stoul(mo[1].str(), nullptr, 16));
    }
    std::fprintf(stderr, "%s: symbol %s not found\n", lblPath.c_str(), name.c_str());
    std::exit(1);
}

void type(Machine& m, const std::string& s) {
    for (char c : s) { m.type_char(uint8_t(c)); m.run_cycles(1000); }
}
void typeLine(Machine& m, const std::string& s) {
    type(m, s);
    type(m, "\r");
    m.run_cycles(300000);
}

void putLE16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(uint8_t(v & 0xFF));
    out.push_back(uint8_t((v >> 8) & 0xFF));
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: %s <firmware.bin> <firmware.lbl> <source.asm> <output.bin>\n", argv[0]);
        return 1;
    }
    const char* firmwarePath = argv[1];
    const char* lblPath = argv[2];
    const char* srcPath = argv[3];
    const char* outPath = argv[4];

    std::ifstream fw(firmwarePath, std::ios::binary);
    if (!fw) { std::fprintf(stderr, "%s: cannot open firmware\n", firmwarePath); return 1; }
    std::vector<uint8_t> img((std::istreambuf_iterator<char>(fw)), std::istreambuf_iterator<char>());
    if (img.size() != 32768) { std::fprintf(stderr, "%s: bad firmware size %zu\n", firmwarePath, img.size()); return 1; }

    uint16_t shellPrompt = readLabel(lblPath, "SHELL_PROMPT");

    std::string out;
    Machine m;
    m.bus.rom.load_image(img.data(), int(img.size()));
    m.on_serial_out = [&](uint8_t c) { out += char(c); };
    m.run_cycles(200000);
    out.clear();

    // Enter the shell, exactly like a real visitor -- SHELL_ENTRY is the
    // one fixed, memorable address (bios.s). Nothing after this point is
    // paced for realism; this is offline tooling, not a live page.
    char runBuf[8];
    std::snprintf(runBuf, sizeof(runBuf), "%XR", kShellEntry);
    type(m, runBuf);
    type(m, "\r");
    m.run_cycles(200000);

    typeLine(m, "NEW");

    std::ifstream src(srcPath);
    if (!src) { std::fprintf(stderr, "%s: cannot open source\n", srcPath); return 1; }
    std::string line;
    int lineNo = 0;
    while (std::getline(src, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        typeLine(m, line);
        lineNo++;
    }
    // The program's real last line -- see this file's header.
    char jmpBuf[24];
    std::snprintf(jmpBuf, sizeof(jmpBuf), "JMP $%04X", shellPrompt);
    typeLine(m, jmpBuf);
    lineNo++;

    // Build-time-only correctness check: this exact typed program must
    // really assemble on the real ROM, or the build fails loudly here
    // instead of shipping a broken Example nobody notices until they
    // click ASM themselves.
    out.clear();
    type(m, "ASM\r");
    m.run_cycles(3000000);
    if (out.find("Ok") == std::string::npos) {
        std::fprintf(stderr, "%s: assembly FAILED (%d source lines) -- ROM said:\n%s\n", srcPath, lineNo, out.c_str());
        return 1;
    }

    // Find the source buffer's real length: scan from SRC_START for the
    // 2-byte 0,0 end-of-buffer sentinel (editor.s's STORE_LINE convention
    // -- see CGOAC6502_REVIEW.md's line-numbered-program-store writeup).
    int srcLen = 0;
    for (int i = 0; i + 1 < 0x1000; i++) {
        if (m.bus.ram[kSrcStart + i] == 0 && m.bus.ram[kSrcStart + i + 1] == 0) {
            srcLen = i + 2;
            break;
        }
    }
    if (srcLen == 0) { std::fprintf(stderr, "%s: never found the source buffer's end sentinel\n", srcPath); return 1; }

    std::vector<uint8_t> outBytes;
    putLE16(outBytes, uint16_t(srcLen));
    for (int i = 0; i < srcLen; i++) outBytes.push_back(m.bus.ram[kSrcStart + i]);

    std::ofstream ofs(outPath, std::ios::binary);
    if (!ofs) { std::fprintf(stderr, "%s: cannot write output\n", outPath); return 1; }
    ofs.write(reinterpret_cast<const char*>(outBytes.data()), std::streamsize(outBytes.size()));
    if (!ofs) { std::fprintf(stderr, "%s: write failed\n", outPath); return 1; }

    std::fprintf(stderr, "%s -> %s (src %d, assembled clean)\n", srcPath, outPath, srcLen);
    return 0;
}
