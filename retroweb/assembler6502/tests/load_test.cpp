// Tests serial LOAD/SAVE (load.s) as shell commands (editor.s DISPATCH_CMD),
// moving the numbered program over the ACIA as decimal-ASCII text.

#include <gtest/gtest.h>

#include "../machine.h"

#include <fstream>
#include <string>
#include <vector>

using namespace machine;

namespace {

// Address from tmp/firmware.lbl
constexpr uint16_t kShellEntry = 0x8000;

constexpr char kFrameStx = 0x02;
constexpr char kFrameEtx = 0x03;

void type(Machine& m, const std::string& s) {
    for (char c : s) { m.type_char(uint8_t(c)); m.run_cycles(1000); }
}

void typeLine(Machine& m, const std::string& s) {
    type(m, s);
    type(m, "\r");
    m.run_cycles(300000);
}

void runAt(Machine& m, uint16_t addr) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%XR", addr);
    type(m, buf);
    type(m, "\r");
    m.run_cycles(200000);
}

Machine boot(std::string& out) {
    std::ifstream f("../../../../cpu6502/rom/tmp/firmware.bin", std::ios::binary);
    if (!f) { ADD_FAILURE() << "firmware.bin not built -- run `make -C .. rom` first"; return Machine(); }
    std::vector<uint8_t> img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    EXPECT_EQ(img.size(), 32768u);

    Machine m;
    m.bus.rom.load_image(img.data(), int(img.size()));
    m.on_serial_out = [&](uint8_t c) { out += char(c); };
    m.run_cycles(200000);
    return m;
}

Machine bootIntoShell(std::string& out) {
    Machine m = boot(out);
    out.clear();
    runAt(m, kShellEntry);
    return m;
}

#define SKIP_UNLESS_ROM_BUILT()                                                                     \
    do {                                                                                             \
        std::ifstream probe("../../../../cpu6502/rom/tmp/firmware.bin", std::ios::binary);           \
        if (!probe) GTEST_SKIP() << "firmware.bin not built -- run `make -C .. rom` first";           \
    } while (0)

TEST(Load, SaveEmitsDecimalNumberedLinesFramedInStxEtx) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    typeLine(m, "10 LDA #$2A");
    typeLine(m, "20 STA $50");

    out.clear();
    type(m, "SAVE\r");
    m.run_cycles(150000);

    auto stxPos = out.find(kFrameStx);
    auto etxPos = out.find(kFrameEtx);
    ASSERT_NE(stxPos, std::string::npos) << "no STX in: " << out;
    ASSERT_NE(etxPos, std::string::npos) << "no ETX in: " << out;
    ASSERT_LT(stxPos, etxPos);

    std::string framed = out.substr(stxPos + 1, etxPos - stxPos - 1);
    EXPECT_EQ(framed, "10 LDA #$2A\r20 STA $50\r");
}

TEST(Load, LoadReplacesTheProgramAndRoundTripsWithSave) {
    // SAVE a program over the ACIA, LOAD the bytes back (minus the STX/ETX frame),
    // and confirm LIST matches
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    typeLine(m, "5 START: LDA #$2A");
    typeLine(m, "10 STA $50");
    typeLine(m, "20 JMP START");

    out.clear();
    type(m, "SAVE\r");
    m.run_cycles(150000);
    auto stxPos = out.find(kFrameStx);
    auto etxPos = out.find(kFrameEtx);
    ASSERT_NE(stxPos, std::string::npos);
    ASSERT_NE(etxPos, std::string::npos);
    std::string saved = out.substr(stxPos + 1, etxPos - stxPos - 1);

    // overwrite the buffer first so LOAD's NEW is what clears it
    typeLine(m, "NEW");
    typeLine(m, "1 DECOY");

    type(m, "LOAD\r");
    m.run_cycles(20000);
    type(m, saved);
    // let the idle timeout (256 * CHLL's ~1275 cycles, load.s) elapse with margin
    m.run_cycles(600000);

    out.clear();
    typeLine(m, "LIST");
    EXPECT_NE(out.find("5 START:  LDA #$2A"), std::string::npos) << "got: " << out;
    EXPECT_NE(out.find("10         STA $50"), std::string::npos) << "got: " << out;
    EXPECT_NE(out.find("20         JMP START"), std::string::npos) << "got: " << out;
    EXPECT_EQ(out.find("DECOY"), std::string::npos) << "decoy line survived: " << out;
}

TEST(Load, LoadEchoesEachIncomingLineOnItsOwnTerminalRowNotOverwritingThePrevious) {
    // SAVE's wire format is bare-CR separated (load.s), but READCHAR echoes raw
    // bytes and a lone CR doesn't advance the row. DO_LOAD adds an LF after each
    // CR (as READLINE_ECHO does), or loaded lines would overwrite each other.
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    out.clear();
    type(m, "LOAD\r");
    m.run_cycles(20000);
    type(m, "10 LDA #$2A\r20 STA $50\r");
    m.run_cycles(600000);

    // every echoed CR must be followed by an LF
    size_t pos = 0;
    int crCount = 0;
    while ((pos = out.find('\r', pos)) != std::string::npos) {
        ASSERT_LT(pos + 1, out.size()) << "trailing bare CR with nothing after it: " << out;
        EXPECT_EQ(out[pos + 1], '\n') << "CR not followed by LF at offset " << pos << ": got: " << out;
        crCount++;
        pos++;
    }
    EXPECT_GE(crCount, 2) << "expected at least the two loaded lines' own CRs: got: " << out;
}

TEST(Load, LoadAutoNumbersAPlainUnnumberedTextImport) {
    // an unnumbered .asm file auto-numbers as at the prompt (PROCESS_LINE)
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    type(m, "LOAD\r");
    m.run_cycles(20000);
    type(m, "LDA #$2A\rSTA $50\r");
    m.run_cycles(600000);

    out.clear();
    typeLine(m, "LIST");
    EXPECT_NE(out.find("10         LDA #$2A"), std::string::npos) << "got: " << out;
    EXPECT_NE(out.find("20         STA $50"), std::string::npos) << "got: " << out;
}

TEST(Load, LoadDoesNotLetAnUnnumberedLineThatReadsLikeACommandHijackTheTransfer) {
    // an imported line reading "RUN" must not run as a command mid-transfer
    // (PROCESS_LINE's LOADMODE gate). CHECK_SYNTAX also rejects it as a mnemonic.
    // The valid lines around it still land in order.
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    type(m, "LOAD\r");
    m.run_cycles(20000);
    type(m, "LDA #$2A\rRUN\rSTA $50\r");
    m.run_cycles(600000);

    // still in the shell, not running raw object code
    EXPECT_NE(out.find(">"), std::string::npos) << "got: " << out;
    // the "RUN" line was rejected, not stored
    EXPECT_NE(out.find("?SYNTAX"), std::string::npos) << "got: " << out;

    out.clear();
    typeLine(m, "LIST");
    // numbered 10/20, since the rejected line consumed no number
    EXPECT_NE(out.find("10         LDA #$2A"), std::string::npos) << "got: " << out;
    EXPECT_NE(out.find("20         STA $50"), std::string::npos) << "got: " << out;
    EXPECT_EQ(out.find("RUN"), std::string::npos) << "the invalid line got stored anyway: got: " << out;
}

TEST(Load, LoadReportsFullOnAnOverlongTransfer) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    type(m, "LOAD\r");
    m.run_cycles(20000);

    // Numbered lines until the 4K source region fills, hitting STORE_LINE's guard.
    // Comment-only filler (CHECK_SYNTAX skips from ';' on), since a real mnemonic
    // would hit the line-number ceiling first. Echo goes through CHLL's per-char
    // delay, so chunks get generous cycles.
    for (int i = 1; i <= 150 && out.find("FULL") == std::string::npos; i++) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%d ; AAAAAAAAAAAAAAAAAAAAAAAAAAAAA\r", i);
        std::string line(buf);
        type(m, line);
        m.run_cycles(int(line.size()) * 2000);
    }
    m.run_cycles(600000);

    EXPECT_NE(out.find("FULL"), std::string::npos) << "got tail: " << out.substr(out.size() > 80 ? out.size() - 80 : 0);
}

} // namespace
