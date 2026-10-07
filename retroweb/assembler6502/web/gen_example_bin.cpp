// Build-time tool: types one Example program into the ROM's resident editor
// headlessly, then dumps the source buffer for app.js's pokeExample() to
// poke into RAM. This sidesteps the Examples-load wedging bug
// (CGOAC6502_REVIEW.md, "Examples-load wedging the CPU"). It also runs the
// ROM's ASM over the source so a broken example fails the build.
//
// Usage: gen_example_bin <firmware.bin> <firmware.lbl> <source.asm> <output.bin>
//
// Output: [2] srcLen (little-endian), then the source buffer from SRC_START
// through the 2-byte 0,0 end sentinel.
//
// Appends "JMP $<SHELL_PROMPT>" as the last line so the program returns to
// the shell instead of running off into zeroed RAM. SHELL_PROMPT comes from
// firmware.lbl since the linker moves it.
#include "../machine.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

using namespace machine;

namespace {

// SRC_START is an editor.s constant, not a label, so firmware.lbl lacks it
constexpr uint16_t kShellEntry = 0x8000;
constexpr uint16_t kSrcStart = 0x3000;

// Reads an "al <hex> .<NAME>" address from firmware.lbl, as gen_entrypoints.py does
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

    // enter the shell at its fixed address (bios.s)
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
    char jmpBuf[24];
    std::snprintf(jmpBuf, sizeof(jmpBuf), "JMP $%04X", shellPrompt);
    typeLine(m, jmpBuf);
    lineNo++;

    // the typed program must assemble on the real ROM or the build fails
    out.clear();
    type(m, "ASM\r");
    m.run_cycles(3000000);
    if (out.find("Ok") == std::string::npos) {
        std::fprintf(stderr, "%s: assembly FAILED (%d source lines) -- ROM said:\n%s\n", srcPath, lineNo, out.c_str());
        return 1;
    }

    // scan for the 0,0 end sentinel (editor.s STORE_LINE)
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
