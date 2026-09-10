// GoogleTest suite for the resident two-pass assembler (cpu6502/rom/editor.s),
// driven through the real shell (SHELL_ENTRY -> ">" prompt) exactly as a
// human typing a program in would -- the old Phase 2 "headless, poke the
// source buffer via Wozmon's own store command" technique predates the
// shell and no longer applies now that the source buffer's real format is
// line-numbered (a 2-byte binary prefix per line, editor.s's STORE_LINE),
// not the old flat unnumbered text a raw poke could produce directly.

#include <gtest/gtest.h>

#include "../machine.h"

#include <fstream>
#include <string>
#include <vector>

using namespace machine;

namespace {

// SHELL_ENTRY's real, built address -- verified against tmp/firmware.lbl
// after each `make -C ../../../cpu6502/rom` (see editor.s's header).
constexpr uint16_t kShellEntry = 0x8000;
constexpr uint16_t kObjStart = 0x0400;

// Types `s` at real per-character pacing. The ACIA has only a one-byte RX
// register -- pushing a second char before the NMI handler has drained the
// first (into SERIAL_BUFFER) overwrites it, a real overrun -- see
// BootsTheRealFirmwareStraightToWozmon in machine_test.cpp for the same
// reasoning.
void type(Machine& m, const std::string& s) {
    for (char c : s) { m.type_char(uint8_t(c)); m.run_cycles(1000); }
}

// Post-CR budget: generous not just for output (this ROM's terse replies)
// but for STORE_LINE's own O(n) buffer scan, which this file's larger
// (100+ line) test programs exercise for real -- each new auto-numbered
// line re-scans every prior one to find the true append point.
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

// Boots the real firmware and enters the shell, with `out` accumulating
// everything the ROM sends back from that point on.
Machine bootIntoShell(std::string& out) {
    std::ifstream f("../../../../cpu6502/rom/tmp/firmware.bin", std::ios::binary);
    if (!f) { ADD_FAILURE() << "firmware.bin not built -- run `make -C .. rom` first"; return Machine(); }
    std::vector<uint8_t> img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    EXPECT_EQ(img.size(), 32768u);

    Machine m;
    m.bus.rom.load_image(img.data(), int(img.size()));
    m.on_serial_out = [&](uint8_t c) { out += char(c); };
    m.run_cycles(200000);
    runAt(m, kShellEntry);
    out.clear();
    return m;
}

// Types each line unnumbered (auto-numbering, editor.s's AUTO_NUMBER,
// preserves entry order via steps of 10) then "ASM". Budgets generously
// (real 1MHz cycles) so even a few-hundred-line program has room for both
// passes to finish within one call.
void assemble(Machine& m, const std::vector<std::string>& lines) {
    for (const auto& l : lines) typeLine(m, l);
    type(m, "ASM\r");
    m.run_cycles(2000000);
}

#define SKIP_UNLESS_ROM_BUILT()                                                                     \
    do {                                                                                             \
        std::ifstream probe("../../../../cpu6502/rom/tmp/firmware.bin", std::ios::binary);           \
        if (!probe) GTEST_SKIP() << "firmware.bin not built -- run `make -C .. rom` first";           \
    } while (0)

TEST(Assembler, AssemblesLabelsBranchesAndAllV1AddressingModes) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    // START: LDA #$05        A9 05
    //        STA $10         85 10
    //        LDX $10         A6 10
    // LOOP:  DEX              CA
    //        BNE LOOP         D0 FD   (target $0406, pc-after $0409 -> -3)
    //        JMP START        4C 00 04
    out.clear();
    assemble(m, {
        "START: LDA #$05",
        "STA $10",
        "LDX $10",
        "LOOP: DEX",
        "BNE LOOP",
        "JMP START",
    });
    EXPECT_NE(out.find("Ok"), std::string::npos) << "got: " << out;

    const uint8_t expected[] = { 0xA9, 0x05, 0x85, 0x10, 0xA6, 0x10, 0xCA, 0xD0, 0xFD, 0x4C, 0x00, 0x04 };
    for (size_t i = 0; i < sizeof(expected); i++) {
        EXPECT_EQ(m.bus.ram[kObjStart + i], expected[i]) << "byte " << i;
    }
}

TEST(Assembler, ResolvesAForwardLabelReference) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    // JMP SKIP     4C 05 04   (SKIP not yet defined -- pass 1 must record it)
    // LDA #$FF     A9 FF
    // SKIP: NOP    EA
    out.clear();
    assemble(m, { "JMP SKIP", "LDA #$FF", "SKIP: NOP" });
    EXPECT_NE(out.find("Ok"), std::string::npos) << "got: " << out;

    const uint8_t expected[] = { 0x4C, 0x05, 0x04, 0xA9, 0xFF, 0xEA };
    for (size_t i = 0; i < sizeof(expected); i++) {
        EXPECT_EQ(m.bus.ram[kObjStart + i], expected[i]) << "byte " << i;
    }
}

TEST(Assembler, AssemblesAbsoluteIndexedAndAccumulatorModes) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    // LDA $1234,X   BD 34 12
    // STA $1234,Y   99 34 12
    // ASL A          0A   (accumulator -- bare mnemonic, no operand)
    // STZ $20,X      74 20
    out.clear();
    assemble(m, { "LDA $1234,X", "STA $1234,Y", "ASL", "STZ $20,X" });
    EXPECT_NE(out.find("Ok"), std::string::npos) << "got: " << out;

    const uint8_t expected[] = { 0xBD, 0x34, 0x12, 0x99, 0x34, 0x12, 0x0A, 0x74, 0x20 };
    for (size_t i = 0; i < sizeof(expected); i++) {
        EXPECT_EQ(m.bus.ram[kObjStart + i], expected[i]) << "byte " << i;
    }
}

TEST(Assembler, ReportsAnUndefinedLabelAsAnErrorWithTheRealLineNumber) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    // An explicit, deliberately non-sequential number -- proves the error
    // report echoes the real program-line number (DO_ASM/CURNUM), not a
    // 1-based physical line count.
    out.clear();
    assemble(m, { "70 JMP NOWHERE" });
    EXPECT_NE(out.find("ERR LINE 70"), std::string::npos) << "got: " << out;
}

TEST(Assembler, RejectsAnUnknownMnemonicImmediatelyAtEntryNotJustAtAsm) {
    // CHECK_SYNTAX (editor.s's STORE_LINE) now catches an unrecognized
    // mnemonic the instant the line is typed, via the same MNEM_LOOKUP
    // PARSE_LINE itself uses for real assembly -- it's never even stored,
    // so ASM's own "ERR LINE n" never gets a chance to see it at all.
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    out.clear();
    typeLine(m, "15 XYZ $10");
    EXPECT_NE(out.find("?SYNTAX"), std::string::npos) << "got: " << out;

    out.clear();
    typeLine(m, "LIST");
    EXPECT_EQ(out.find("XYZ"), std::string::npos) << "bad line got stored anyway: got: " << out;
}

TEST(Assembler, RejectsABranchTargetOutOfTheSignedByteRange) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    // 200 NOPs put FAR well past a signed 8-bit branch's +/-128 reach from
    // the BEQ that follows.
    std::vector<std::string> lines = { "FAR: NOP" };
    for (int i = 0; i < 200; i++) lines.push_back("NOP");
    lines.push_back("BEQ FAR");

    out.clear();
    assemble(m, lines);
    EXPECT_NE(out.find("ERR LINE"), std::string::npos) << "got: " << out;
    EXPECT_EQ(out.find("Ok"), std::string::npos);
}

TEST(Assembler, PromotesAZeroPageLiteralToAbsoluteWhenOnlyAbsoluteIsSupported) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    // JSR has no zero-page encoding on real 6502/65C02 -- it always takes
    // a full 16-bit target. A 2-hex-digit ("$10") literal operand must
    // still assemble, widened to JSR's only real form (absolute, $20).
    out.clear();
    assemble(m, { "JSR $10" });
    EXPECT_NE(out.find("Ok"), std::string::npos) << "got: " << out;
    EXPECT_EQ(m.bus.ram[kObjStart + 0], 0x20);
    EXPECT_EQ(m.bus.ram[kObjStart + 1], 0x10);
    EXPECT_EQ(m.bus.ram[kObjStart + 2], 0x00);
}

TEST(Assembler, InlineCommentsAreIgnoredByTheAssembler) {
    // editor.s's own header has always documented "LABEL: MNEMONIC OPERAND
    // ; comment" as the real grammar, and ASM_READLINE already strips
    // everything from a ';' to end-of-line before PARSE_LINE ever sees it
    // -- this was simply never covered by a test until now.
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    out.clear();
    assemble(m, { "LDA #$2A ; load the answer", "STA $50 ; stash it" });
    ASSERT_NE(out.find("Ok"), std::string::npos) << "got: " << out;
    EXPECT_EQ(m.bus.ram[kObjStart + 0], 0xA9);
    EXPECT_EQ(m.bus.ram[kObjStart + 1], 0x2A);
    EXPECT_EQ(m.bus.ram[kObjStart + 2], 0x85);
    EXPECT_EQ(m.bus.ram[kObjStart + 3], 0x50);
}

TEST(Assembler, AFullLineCommentAssemblesAsANoOp) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    out.clear();
    assemble(m, { "LDA #$2A", "; just a comment, no code at all", "STA $50" });
    ASSERT_NE(out.find("Ok"), std::string::npos) << "got: " << out;
    // No bytes emitted for the comment line -- STA lands right after LDA,
    // exactly as if the comment line weren't there.
    EXPECT_EQ(m.bus.ram[kObjStart + 0], 0xA9);
    EXPECT_EQ(m.bus.ram[kObjStart + 1], 0x2A);
    EXPECT_EQ(m.bus.ram[kObjStart + 2], 0x85);
    EXPECT_EQ(m.bus.ram[kObjStart + 3], 0x50);
}

TEST(Assembler, JsrPrintCharReachesTheTerminal) {
    // PRINT_CHAR ($8003) is one of bios.s's fixed, pinned OS-call jump-
    // table entries -- callable by JSR from a user program exactly like
    // any other fixed address (RUN's own resume convention already relies
    // on this same "hand-typed hex address" idiom).
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    out.clear();
    assemble(m, { "LDA #$41", "JSR $8003" });
    ASSERT_NE(out.find("Ok"), std::string::npos) << "got: " << out;

    out.clear();
    type(m, "RUN\r");
    m.run_cycles(20000);
    EXPECT_NE(out.find('A'), std::string::npos) << "got: " << out;
}

TEST(Assembler, JsrPrintStrReachesTheTerminal) {
    // PRINT_STR ($8006) takes A/Y = lo/hi of a NUL-terminated string. This
    // v1 assembler has no data directive to embed one directly (hex-
    // literal operands only -- see the Help panel's "v1 assembler scope"),
    // so a real program has to build one byte-by-byte via LDA/STA first;
    // this test does the same poke a real typed-in program's own STA
    // sequence would produce, then assembles just the JSR against it.
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    const uint16_t strAddr = 0x0600;
    const char msg[] = "HI";
    for (size_t i = 0; i <= sizeof(msg) - 1; i++) m.bus.ram[strAddr + i] = uint8_t(msg[i]);   // includes the NUL

    out.clear();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "LDA #$%02X", strAddr & 0xFF);
    std::string loLine = buf;
    std::snprintf(buf, sizeof(buf), "LDY #$%02X", (strAddr >> 8) & 0xFF);
    std::string hiLine = buf;
    assemble(m, { loLine, hiLine, "JSR $8006" });
    ASSERT_NE(out.find("Ok"), std::string::npos) << "got: " << out;

    out.clear();
    type(m, "RUN\r");
    m.run_cycles(20000);
    EXPECT_NE(out.find("HI"), std::string::npos) << "got: " << out;
}

TEST(Assembler, JsrLcdPutcAndPutsReachTheLcd) {
    // LCD_PUTC ($8009) and LCD_PUTS ($800C) -- same fixed jump-table
    // pattern, out the HD44780 instead of the ACIA. LCD_CLEAR ($800F)
    // first, so this doesn't depend on whatever RESET's own boot banner
    // already left on the display.
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    const uint16_t strAddr = 0x0600;
    const char msg[] = "OK";
    for (size_t i = 0; i <= sizeof(msg) - 1; i++) m.bus.ram[strAddr + i] = uint8_t(msg[i]);

    out.clear();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "LDA #$%02X", strAddr & 0xFF);
    std::string loLine = buf;
    std::snprintf(buf, sizeof(buf), "LDY #$%02X", (strAddr >> 8) & 0xFF);
    std::string hiLine = buf;
    assemble(m, { "JSR $800F", "LDA #$58", "JSR $8009", loLine, hiLine, "JSR $800C" });
    ASSERT_NE(out.find("Ok"), std::string::npos) << "got: " << out;

    out.clear();
    type(m, "RUN\r");
    m.run_cycles(30000);
    EXPECT_EQ(m.lcd.text[0][0], 'X');    // LCD_PUTC wrote 'X' ($58) at the cursor's start position
    EXPECT_EQ(m.lcd.text[0][1], 'O');    // then LCD_PUTS continued from there
    EXPECT_EQ(m.lcd.text[0][2], 'K');
}

} // namespace
