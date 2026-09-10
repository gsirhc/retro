// GoogleTest suite for serial LOAD/SAVE (cpu6502/rom/load.s) as shell
// commands (cpu6502/rom/editor.s's DISPATCH_CMD) -- moving the numbered
// program across the real ACIA link as decimal-ASCII text. Driven
// entirely through the simulated ACIA/terminal, exactly as a human typing
// "LOAD"/"SAVE" at the shell's own prompt would.

#include <gtest/gtest.h>

#include "../machine.h"

#include <fstream>
#include <string>
#include <vector>

using namespace machine;

namespace {

// Real, built address -- verified against tmp/firmware.lbl after each
// `make -C ../../../cpu6502/rom` (see editor.s's header).
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
    // The end-to-end scenario: SAVE a program out over the ACIA, then LOAD
    // the exact bytes SAVE emitted (minus the STX/ETX frame, same as the
    // browser's capture would strip) back in, and confirm LIST matches.
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

    // Overwrite the buffer with something else first, so LOAD's own NEW
    // (not leftover coincidence) is what actually clears it.
    typeLine(m, "NEW");
    typeLine(m, "1 DECOY");

    type(m, "LOAD\r");
    m.run_cycles(20000);
    type(m, saved);
    // Let the idle timeout (256 * CHLL's ~1275-cycle delay, load.s) elapse
    // with no further bytes -- generous margin over the real threshold.
    m.run_cycles(600000);

    out.clear();
    typeLine(m, "LIST");
    EXPECT_NE(out.find("5 START:  LDA #$2A"), std::string::npos) << "got: " << out;
    EXPECT_NE(out.find("10         STA $50"), std::string::npos) << "got: " << out;
    EXPECT_NE(out.find("20         JMP START"), std::string::npos) << "got: " << out;
    EXPECT_EQ(out.find("DECOY"), std::string::npos) << "decoy line survived: " << out;
}

TEST(Load, LoadEchoesEachIncomingLineOnItsOwnTerminalRowNotOverwritingThePrevious) {
    // The wire format SAVE emits (and LOAD reads back) is deliberately
    // bare-CR-separated (load.s's own header) -- but READCHAR (bios.s)
    // unconditionally echoes every raw byte it reads, for every caller,
    // including DO_LOAD's ingestion loop. A lone CR on a real terminal
    // returns the cursor to column 0 *without* advancing to the next row,
    // so without DO_LOAD adding its own LF after each received CR (the
    // same fix READLINE_ECHO already applies for the interactive prompt),
    // each loaded line's echoed text would visually overwrite the
    // previous one in place instead of appearing on its own line -- a
    // real bug a flat byte-content check alone would never catch, since
    // it's about terminal cursor positioning, not the bytes' presence.
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    out.clear();
    type(m, "LOAD\r");
    m.run_cycles(20000);
    type(m, "10 LDA #$2A\r20 STA $50\r");
    m.run_cycles(600000);

    // Every CR this transfer echoed back must be immediately followed by
    // an LF -- proves each line actually advanced the terminal to its own
    // row rather than returning to column 0 and colliding with the next.
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
    // A human-typed .asm file with no line numbers at all -- LOAD must
    // still accept it, auto-numbering each line exactly like typing it
    // unnumbered at the shell prompt would (PROCESS_LINE's shared path).
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
    // A plain unnumbered import whose content happens to read "RUN" must
    // never be misread as the shell's RUN command mid-transfer -- exactly
    // the failure PROCESS_LINE's LOADMODE gate exists to prevent (see its
    // header comment in editor.s). "RUN" also isn't a valid 6502 mnemonic,
    // so CHECK_SYNTAX (STORE_LINE) now rejects it as a program line too,
    // rather than silently storing it as garbage the way unchecked entry
    // used to. Either way, the transfer itself must not derail: the two
    // real, valid lines around it still land, in order.
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    type(m, "LOAD\r");
    m.run_cycles(20000);
    type(m, "LDA #$2A\rRUN\rSTA $50\r");
    m.run_cycles(600000);

    // Never hijacked into actually running anything -- still sitting in
    // the shell, not off executing raw object code.
    EXPECT_NE(out.find(">"), std::string::npos) << "got: " << out;
    // The bad "RUN" line was rejected, not silently stored.
    EXPECT_NE(out.find("?SYNTAX"), std::string::npos) << "got: " << out;

    out.clear();
    typeLine(m, "LIST");
    // The two real lines made it in, in order, auto-numbered 10/20 (not
    // 10/30) since the rejected middle line never actually consumed a
    // number -- proof the transfer continued normally around it.
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

    // Many distinct numbered lines, long enough combined to exhaust the
    // whole 4K source region -- a real overlong transfer would hit the
    // same STORE_LINE guard. A comment-only line (CHECK_SYNTAX skips
    // everything from ';' on, same as ASM itself) is filler here -- a
    // real mnemonic repeated this many times would hit STORE_LINE's own
    // line-number ceiling long before the buffer itself filled up. Every
    // received character is still echoed via READCHAR before any bounds
    // check runs, and that echo goes through a real per-character ACIA
    // delay (CHLL, bios.s) -- generous cycles per chunk, same reasoning
    // as editor_test.cpp's equivalent case.
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
