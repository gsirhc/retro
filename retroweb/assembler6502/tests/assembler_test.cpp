// Tests the ROM's two-pass assembler (editor.s) through the shell, since the
// source buffer is line-numbered (STORE_LINE) and can't be poked flat.

#include <gtest/gtest.h>

#include "../machine.h"

#include <fstream>
#include <string>
#include <vector>

using namespace machine;

namespace {

// SHELL_ENTRY address from tmp/firmware.lbl
constexpr uint16_t kShellEntry = 0x8000;
constexpr uint16_t kObjStart = 0x0400;

// One-byte ACIA RX register: the NMI handler needs cycles to drain between chars
void type(Machine& m, const std::string& s) {
    for (char c : s) { m.type_char(uint8_t(c)); m.run_cycles(1000); }
}

// Post-CR budget covers STORE_LINE's O(n) scan to find the append point
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

// Boots the firmware into the shell; `out` collects ROM output
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

// Types each line unnumbered (AUTO_NUMBER steps by 10) then "ASM"
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

    // non-sequential number: the error echoes the program-line number (DO_ASM/CURNUM)
    out.clear();
    assemble(m, { "70 JMP NOWHERE" });
    EXPECT_NE(out.find("ERR LINE 70"), std::string::npos) << "got: " << out;
}

TEST(Assembler, RejectsAnUnknownMnemonicImmediatelyAtEntryNotJustAtAsm) {
    // CHECK_SYNTAX (STORE_LINE) rejects an unknown mnemonic at entry, so it's never stored
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

    // 200 NOPs put FAR past a branch's +/-128 reach
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

    // JSR has no zero-page form, so "$10" must widen to absolute
    out.clear();
    assemble(m, { "JSR $10" });
    EXPECT_NE(out.find("Ok"), std::string::npos) << "got: " << out;
    EXPECT_EQ(m.bus.ram[kObjStart + 0], 0x20);
    EXPECT_EQ(m.bus.ram[kObjStart + 1], 0x10);
    EXPECT_EQ(m.bus.ram[kObjStart + 2], 0x00);
}

TEST(Assembler, InlineCommentsAreIgnoredByTheAssembler) {
    // ASM_READLINE strips from ';' to end of line
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
    // a comment line emits no bytes
    EXPECT_EQ(m.bus.ram[kObjStart + 0], 0xA9);
    EXPECT_EQ(m.bus.ram[kObjStart + 1], 0x2A);
    EXPECT_EQ(m.bus.ram[kObjStart + 2], 0x85);
    EXPECT_EQ(m.bus.ram[kObjStart + 3], 0x50);
}

TEST(Assembler, JsrPrintCharReachesTheTerminal) {
    // PRINT_CHAR ($8003) is a fixed bios.s jump-table entry
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
    // PRINT_STR ($8006) takes A/Y = lo/hi of a NUL-terminated string; poke one into RAM and assemble just the JSR
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    const uint16_t strAddr = 0x0600;
    const char msg[] = "HI";
    for (size_t i = 0; i <= sizeof(msg) - 1; i++) m.bus.ram[strAddr + i] = uint8_t(msg[i]);

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
    // LCD_PUTC ($8009) and LCD_PUTS ($800C); LCD_CLEAR ($800F) first to drop the boot banner
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
    EXPECT_EQ(m.lcd.text[0][0], 'X');
    EXPECT_EQ(m.lcd.text[0][1], 'O');
    EXPECT_EQ(m.lcd.text[0][2], 'K');
}

TEST(Assembler, ByteDirectiveEmitsAQuotedStringVerbatim) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    out.clear();
    assemble(m, { "MSG: .BYTE \"HI\"" });
    EXPECT_NE(out.find("Ok"), std::string::npos) << "got: " << out;
    EXPECT_EQ(m.bus.ram[kObjStart + 0], 'H');
    EXPECT_EQ(m.bus.ram[kObjStart + 1], 'I');
}

TEST(Assembler, ByteDirectiveEmitsHexLiteralsAndMixesWithAString) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    out.clear();
    assemble(m, { "MSG: .BYTE \"HI\",$0D,$0A,$00" });
    EXPECT_NE(out.find("Ok"), std::string::npos) << "got: " << out;
    const uint8_t expected[] = { 'H', 'I', 0x0D, 0x0A, 0x00 };
    for (size_t i = 0; i < sizeof(expected); i++) {
        EXPECT_EQ(m.bus.ram[kObjStart + i], expected[i]) << "byte " << i;
    }
}

TEST(Assembler, ByteDirectiveAdvancesThePcSoALaterLabelResolvesPastIt) {
    // .BYTE must advance PC like an instruction's ASIZE, or later labels shift
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    // MSG: .BYTE "HI",$00   3 bytes at $0400-$0402
    // NEXT: NOP             1 byte at $0403
    // JMP NEXT              3 bytes at $0404-$0406 -> 4C 03 04
    out.clear();
    assemble(m, { "MSG: .BYTE \"HI\",$00", "NEXT: NOP", "JMP NEXT" });
    EXPECT_NE(out.find("Ok"), std::string::npos) << "got: " << out;
    EXPECT_EQ(m.bus.ram[kObjStart + 3], 0xEA);
    EXPECT_EQ(m.bus.ram[kObjStart + 4], 0x4C);
    EXPECT_EQ(m.bus.ram[kObjStart + 5], 0x03);
    EXPECT_EQ(m.bus.ram[kObjStart + 6], 0x04);
}

TEST(Assembler, ByteDirectiveStringIsPrintableViaAnIndexedLoop) {
    // no <// >> operators, so LDA MSG,X walks the string; assemble, RUN, check terminal output
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    out.clear();
    assemble(m, {
        "MSG: .BYTE \"HI\",$00",
        "LDX #$00",
        "LOOP: LDA MSG,X",
        "BEQ DONE",
        "JSR $8003",
        "INX",
        "BRA LOOP",
        "DONE: NOP",
    });
    ASSERT_NE(out.find("Ok"), std::string::npos) << "got: " << out;

    out.clear();
    type(m, "RUN\r");
    m.run_cycles(50000);
    EXPECT_NE(out.find("HI"), std::string::npos) << "got: " << out;
}

TEST(Assembler, ByteDirectiveRejectsAnEmptyOperand) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    out.clear();
    typeLine(m, "10 .BYTE");
    EXPECT_NE(out.find("?SYNTAX"), std::string::npos) << "got: " << out;
}

TEST(Assembler, ByteDirectiveRejectsAnUnterminatedString) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    out.clear();
    typeLine(m, "10 .BYTE \"HI");
    EXPECT_NE(out.find("?SYNTAX"), std::string::npos) << "got: " << out;
}

TEST(Assembler, ByteDirectiveListsAsOneUnbrokenKeywordNotSplitAtTheThirdChar) {
    // LIST's PRINT_ENTRY assumes a 3-char mnemonic field; ".BYTE" must not split into ".BY" + "TE"
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    out.clear();
    typeLine(m, "MSG: .BYTE \"HI\",$00");
    out.clear();
    typeLine(m, "LIST");
    EXPECT_NE(out.find(".BYTE \"HI\",$00"), std::string::npos) << "got: " << out;
    EXPECT_EQ(out.find(".BY TE"), std::string::npos) << "got: " << out;
}

TEST(Assembler, ByteDirectiveRejectsAnOversizedHexLiteral) {
    // $xx is a byte literal; 3+ hex digits can't fit
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    out.clear();
    typeLine(m, "10 .BYTE $123");
    EXPECT_NE(out.find("?SYNTAX"), std::string::npos) << "got: " << out;
}

} // namespace
