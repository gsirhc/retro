// Tests the command shell in editor.s: program store, command dispatcher,
// auto-numbering. Entered via a typed "<addr>R" into SHELL_ENTRY, then driven
// at the ">" prompt.

#include <gtest/gtest.h>

#include "../machine.h"

#include <fstream>
#include <string>
#include <vector>

using namespace machine;

namespace {

// Addresses from tmp/firmware.lbl
constexpr uint16_t kShellEntry = 0x8000;
constexpr uint16_t kObjStart = 0x0400;

// One-byte ACIA RX register: the NMI handler needs cycles to drain between chars
void type(Machine& m, const std::string& s) {
    for (char c : s) { m.type_char(uint8_t(c)); m.run_cycles(1000); }
}

// Post-CR budget: output goes through CHLL's per-char delay (~1275 cycles),
// so 300000 covers ~200 chars
void typeLine(Machine& m, const std::string& s) {
    type(m, s);
    type(m, "\r");
    m.run_cycles(300000);
}

// Types "<addr>R\r", the Wozmon run command
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

// Boots and enters the shell
Machine bootIntoShell(std::string& out) {
    Machine m = boot(out);
    out.clear();
    runAt(m, kShellEntry);
    return m;
}

// Reads a label address from tmp/firmware.lbl. SHELL_PROMPT moves whenever
// editor.s changes, unlike SHELL_ENTRY.
uint16_t labelAddr(const std::string& name) {
    std::ifstream f("../../../../cpu6502/rom/tmp/firmware.lbl");
    std::string line;
    while (std::getline(f, line)) {
        auto dot = line.rfind('.' + name);
        if (dot == std::string::npos || dot + 1 + name.size() != line.size()) continue;
        return uint16_t(std::stoul(line.substr(3, 6), nullptr, 16));
    }
    ADD_FAILURE() << "label not found in firmware.lbl: " << name;
    return 0;
}

#define SKIP_UNLESS_ROM_BUILT()                                                                     \
    do {                                                                                             \
        std::ifstream probe("../../../../cpu6502/rom/tmp/firmware.bin", std::ios::binary);           \
        if (!probe) GTEST_SKIP() << "firmware.bin not built -- run `make -C .. rom` first";           \
    } while (0)

TEST(Editor, ShellEntryPrintsBannerAndPrompt) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);
    EXPECT_NE(out.find("6502 ASSEMBLY CODER"), std::string::npos) << "got: " << out;
    EXPECT_NE(out.find(">"), std::string::npos) << "got: " << out;
}

TEST(Editor, RejectsBadSyntaxImmediatelyAtEntryWithoutCheckingLabels) {
    // CHECK_SYNTAX (STORE_LINE) rejects bad mnemonics/operands at entry but
    // doesn't resolve labels, so forward references are accepted.
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    out.clear();
    typeLine(m, "10 XYZZY $50");
    EXPECT_NE(out.find("?SYNTAX"), std::string::npos) << "got: " << out;

    out.clear();
    typeLine(m, "20 JMP FORWARD");   // FORWARD isn't defined until line 30
    EXPECT_EQ(out.find("?SYNTAX"), std::string::npos) << "got: " << out;
    typeLine(m, "30 FORWARD: NOP");

    out.clear();
    typeLine(m, "LIST");
    EXPECT_EQ(out.find("XYZZY"), std::string::npos) << "the bad line got stored anyway: got: " << out;
    EXPECT_NE(out.find("20         JMP FORWARD"), std::string::npos) << "got: " << out;
    EXPECT_NE(out.find("30 FORWARD: NOP"), std::string::npos) << "got: " << out;
}

TEST(Editor, BackspaceErasesTheCharacterNotJustTheCursor) {
    // READCHAR echoes the raw backspace; READLINE_ECHO adds an erase so ghost
    // text doesn't linger. Checked on the raw byte stream.
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    out.clear();
    type(m, "10 LDA");
    m.type_char(0x08);   // backspace over the trailing 'A'
    m.run_cycles(50000);

    // READCHAR's BS echo, then READLINE_ECHO's space and second BS
    EXPECT_NE(out.find("\x08 \x08"), std::string::npos) << "got: " << out;
}

TEST(Editor, DelAlsoErasesDestructivelyAndDoesNotCorruptTheStoredLine) {
    // xterm.js Backspace sends DEL ($7F), not BS. READLINE_ECHO must erase on it
    // instead of storing $7F in LINE_BUF.
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    out.clear();
    type(m, "10 LDA");
    m.type_char(0x7F);   // DEL -- the real browser Backspace byte
    m.run_cycles(50000);

    // READCHAR echoes the raw $7F, READLINE_ECHO supplies the leading BS, then
    // the shared erase
    EXPECT_NE(out.find("\x7F\x08 \x08"), std::string::npos) << "got: " << out;

    // the corrected line stores and lists with no stray $7F. LDX needs an
    // operand to pass CHECK_SYNTAX.
    type(m, "X #$05\r");   // finish the line as "10 LDX #$05"
    m.run_cycles(300000);
    out.clear();
    typeLine(m, "LIST");
    EXPECT_NE(out.find("10         LDX #$05"), std::string::npos) << "got: " << out;
    EXPECT_EQ(out.find("\x7F"), std::string::npos) << "a stray DEL byte leaked into the listing: got: " << out;
}

TEST(Editor, BackspaceOnAnEmptyLineIsANoOp) {
    // Y=0: neither backspace byte touches LINE_BUF or emits the erase, only
    // READCHAR's echo
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    out.clear();
    m.type_char(0x08);
    m.run_cycles(50000);
    EXPECT_EQ(out.find(" \x08"), std::string::npos) << "got: " << out;

    out.clear();
    m.type_char(0x7F);
    m.run_cycles(50000);
    EXPECT_EQ(out.find(" \x08"), std::string::npos) << "got: " << out;
    EXPECT_EQ(out.find("\x08 "), std::string::npos) << "got: " << out;

    // shell still works afterward
    out.clear();
    typeLine(m, "10 NOP");
    out.clear();
    typeLine(m, "LIST");
    EXPECT_NE(out.find("10         NOP"), std::string::npos) << "got: " << out;
}

TEST(Editor, ListFormatsLabelMnemonicOperandAndCommentIntoAlignedColumns) {
    // PRINT_ENTRY realigns stored text into fixed columns for LIST/EDIT
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    typeLine(m, "10 START: LDA #$2A ; load the answer");
    typeLine(m, "20 STA $50");
    typeLine(m, "30 NOP");
    typeLine(m, "40 ; a full-line comment, no code at all");

    out.clear();
    typeLine(m, "LIST");
    // label padded to the mnemonic column
    EXPECT_NE(out.find("10 START:  LDA #$2A      ; LOAD THE ANSWER"), std::string::npos) << "got: " << out;
    EXPECT_NE(out.find("20         STA $50"), std::string::npos) << "got: " << out;
    EXPECT_NE(out.find("30         NOP"), std::string::npos) << "got: " << out;
    // comment-only line realigned to the comment column
    EXPECT_NE(out.find("40                       ; A FULL-LINE COMMENT, NO CODE AT ALL"), std::string::npos) << "got: " << out;
}

TEST(Editor, ExplicitlyNumberedLinesListBackInSortedOrder) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    // out-of-order entry; LIST sorts by number
    typeLine(m, "20 STA $50");
    typeLine(m, "10 LDA #$2A");
    typeLine(m, "30 JMP $10");

    out.clear();
    typeLine(m, "LIST");
    auto p10 = out.find("10         LDA #$2A");
    auto p20 = out.find("20         STA $50");
    auto p30 = out.find("30         JMP $10");
    ASSERT_NE(p10, std::string::npos) << "got: " << out;
    ASSERT_NE(p20, std::string::npos) << "got: " << out;
    ASSERT_NE(p30, std::string::npos) << "got: " << out;
    EXPECT_LT(p10, p20);
    EXPECT_LT(p20, p30);
}

TEST(Editor, UnnumberedLinesAutoNumberByTen) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    typeLine(m, "LDA #$2A");
    typeLine(m, "STA $50");

    out.clear();
    typeLine(m, "LIST");
    EXPECT_NE(out.find("10         LDA #$2A"), std::string::npos) << "got: " << out;
    EXPECT_NE(out.find("20         STA $50"), std::string::npos) << "got: " << out;
}

TEST(Editor, RetypingAnExistingNumberReplacesThatLine) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    typeLine(m, "10 LDA #$2A");
    typeLine(m, "20 STA $50");
    typeLine(m, "10 LDA #$FF");   // replace line 10

    out.clear();
    typeLine(m, "LIST");
    EXPECT_NE(out.find("10         LDA #$FF"), std::string::npos) << "got: " << out;
    EXPECT_EQ(out.find("10         LDA #$2A"), std::string::npos) << "old content survived: " << out;
    EXPECT_NE(out.find("20         STA $50"), std::string::npos) << "got: " << out;
}

TEST(Editor, ABareLineNumberDeletesThatLine) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    typeLine(m, "10 LDA #$2A");
    typeLine(m, "20 STA $50");
    typeLine(m, "10");            // delete line 10

    out.clear();
    typeLine(m, "LIST");
    EXPECT_EQ(out.find("LDA"), std::string::npos) << "line 10 survived: " << out;
    EXPECT_NE(out.find("20         STA $50"), std::string::npos) << "got: " << out;
}

TEST(Editor, InsertingBetweenExistingLinesNeedsNoRenumbering) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    typeLine(m, "10 LDA #$2A");
    typeLine(m, "20 STA $50");
    typeLine(m, "15 NOP");        // insert between 10 and 20

    out.clear();
    typeLine(m, "LIST");
    auto p10 = out.find("10         LDA #$2A");
    auto p15 = out.find("15         NOP");
    auto p20 = out.find("20         STA $50");
    ASSERT_NE(p10, std::string::npos);
    ASSERT_NE(p15, std::string::npos);
    ASSERT_NE(p20, std::string::npos);
    EXPECT_LT(p10, p15);
    EXPECT_LT(p15, p20);
}

TEST(Editor, ZeroIsARejectedLineNumber) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    out.clear();
    typeLine(m, "0 LDA #$2A");
    EXPECT_NE(out.find("?LINE"), std::string::npos) << "got: " << out;
}

TEST(Editor, ListWithAnArgumentStartsFromThatLine) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    typeLine(m, "10 LDA #$2A");
    typeLine(m, "20 STA $50");
    typeLine(m, "30 NOP");

    out.clear();
    typeLine(m, "LIST 20");
    EXPECT_EQ(out.find("LDA"), std::string::npos) << "line 10 should be skipped: " << out;
    EXPECT_NE(out.find("20         STA $50"), std::string::npos) << "got: " << out;
    EXPECT_NE(out.find("30         NOP"), std::string::npos) << "got: " << out;
}

TEST(Editor, ListPagesEveryTwentyLinesAndAnyKeyContinues) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    // 25 lines (10..250), more than one LIST_PAGE (20)
    for (int i = 0; i < 25; i++) typeLine(m, "NOP");

    out.clear();
    type(m, "LIST\r");
    // ~17 chars per line at CHLL's per-char delay; generous for a 20-line page
    m.run_cycles(900000);
    EXPECT_NE(out.find("--MORE--"), std::string::npos) << "got tail: " << out.substr(out.size() > 200 ? out.size() - 200 : 0);
    // line 250 still pending behind the pause
    EXPECT_EQ(out.find("250         NOP"), std::string::npos) << "printed past the page break: " << out;

    // any key continues
    type(m, " ");
    m.run_cycles(300000);
    EXPECT_NE(out.find("250         NOP"), std::string::npos) << "got: " << out;
}

TEST(Editor, EditShowsTheLineThenReplacesItWithFreshlyTypedText) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    typeLine(m, "10 LDA #$2A");
    typeLine(m, "20 STA $50");

    out.clear();
    type(m, "EDIT 10\r");
    m.run_cycles(300000);
    EXPECT_NE(out.find("10         LDA #$2A"), std::string::npos) << "did not show the old line: " << out;

    typeLine(m, "LDA #$FF");    // the retyped replacement -- no number needed

    out.clear();
    typeLine(m, "LIST");
    EXPECT_NE(out.find("10         LDA #$FF"), std::string::npos) << "got: " << out;
    EXPECT_EQ(out.find("$2A"), std::string::npos) << "got: " << out;
}

TEST(Editor, EditWithBlankRetypeDeletesTheLine) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    typeLine(m, "10 LDA #$2A");
    typeLine(m, "20 STA $50");

    type(m, "EDIT 10\r");
    m.run_cycles(300000);
    typeLine(m, "");             // blank retype -- delete

    out.clear();
    typeLine(m, "LIST");
    EXPECT_EQ(out.find("LDA"), std::string::npos) << "line 10 survived: " << out;
    EXPECT_NE(out.find("20         STA $50"), std::string::npos) << "got: " << out;
}

TEST(Editor, EditOnANonexistentLineReportsAnError) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    out.clear();
    typeLine(m, "EDIT 99");
    EXPECT_NE(out.find("?LINE"), std::string::npos) << "got: " << out;
}

TEST(Editor, NewClearsTheProgram) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    typeLine(m, "10 LDA #$2A");

    out.clear();
    typeLine(m, "NEW");
    EXPECT_NE(out.find("Ok"), std::string::npos) << "got: " << out;

    out.clear();
    typeLine(m, "LIST");
    EXPECT_EQ(out.find("LDA"), std::string::npos) << "program was not cleared: " << out;
}

TEST(Editor, NewClearsAssembledObjectCodeTooSoAStaleRunFailsCleanly) {
    // NEW plants the "?NO PROGRAM" RUN trap (PLANT_NOPROG_TRAP) so RUN can't
    // re-execute a stale assembly.
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    // ends with JMP SHELL_PROMPT so the shell is alive afterward
    typeLine(m, "10 LDA #$2A");
    typeLine(m, "20 STA $50");
    char buf[16];
    std::snprintf(buf, sizeof(buf), "JMP $%X", labelAddr("SHELL_PROMPT"));
    typeLine(m, std::string("30 ") + buf);

    out.clear();
    typeLine(m, "ASM");
    ASSERT_NE(out.find("Ok"), std::string::npos) << "got: " << out;

    // seed $50 with a sentinel; RUN must overwrite it with $2A
    m.bus.ram[0x50] = 0xFF;
    out.clear();
    typeLine(m, "RUN");
    m.run_cycles(50000);
    ASSERT_EQ(m.bus.ram[0x50], 0x2A) << "sanity check: the program didn't even run the first time";
    ASSERT_NE(out.find(">"), std::string::npos) << "expected the shell back at its own prompt: got: " << out;

    typeLine(m, "NEW");

    // object region holds the JMP ERR_NOPROG trap ($4C)
    EXPECT_EQ(m.bus.ram[kObjStart], 0x4C) << "expected the JMP-to-error trap, not the old program";

    // re-seed and RUN with no ASM: the old program must not run, the shell
    // reports an error and reprompts
    m.bus.ram[0x50] = 0xFF;
    out.clear();
    typeLine(m, "RUN");
    m.run_cycles(20000);
    EXPECT_EQ(m.bus.ram[0x50], 0xFF) << "stale program from before NEW executed anyway";
    EXPECT_NE(out.find("?NO PROGRAM"), std::string::npos) << "got: " << out;
    EXPECT_NE(out.find(">"), std::string::npos) << "expected the shell back at its own prompt: got: " << out;
}

TEST(Editor, RunWithNothingEverAssembledReportsNoProgram) {
    // fresh entry then RUN: the object region would be zeroed RAM (BRK), so the
    // trap must be planted (PLANT_NOPROG_TRAP)
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    out.clear();
    typeLine(m, "RUN");
    m.run_cycles(20000);
    EXPECT_NE(out.find("?NO PROGRAM"), std::string::npos) << "got: " << out;
    EXPECT_NE(out.find(">"), std::string::npos) << "expected the shell back at its own prompt: got: " << out;
}

TEST(Editor, FailedAsmAlsoTrapsRunEvenAfterAPreviousGoodAssembly) {
    // a failed re-ASM must also invalidate the old object code (DO_ASM da_err
    // replants the trap)
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    typeLine(m, "10 LDA #$2A");
    typeLine(m, "20 STA $50");
    out.clear();
    typeLine(m, "ASM");
    ASSERT_NE(out.find("Ok"), std::string::npos) << "got: " << out;

    // Break it: an undefined-label line.
    typeLine(m, "30 JMP UNDEFINED");
    out.clear();
    typeLine(m, "ASM");
    ASSERT_NE(out.find("ERR LINE"), std::string::npos) << "got: " << out;

    m.bus.ram[0x50] = 0xFF;
    out.clear();
    typeLine(m, "RUN");
    m.run_cycles(20000);
    EXPECT_EQ(m.bus.ram[0x50], 0xFF) << "the previous good assembly ran anyway after a failed re-ASM";
    EXPECT_NE(out.find("?NO PROGRAM"), std::string::npos) << "got: " << out;
}

TEST(Editor, QuitAndReenteringTheShellStillPreservesARealAssembledProgram) {
    // re-entering the shell must not clobber a good assembly (SHELL_START gates
    // on HASOBJ)
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    typeLine(m, "10 LDA #$2A");
    typeLine(m, "20 STA $50");
    out.clear();
    typeLine(m, "ASM");
    ASSERT_NE(out.find("Ok"), std::string::npos) << "got: " << out;

    typeLine(m, "QUIT");
    runAt(m, kShellEntry);

    m.bus.ram[0x50] = 0xFF;
    out.clear();
    typeLine(m, "RUN");
    m.run_cycles(20000);
    EXPECT_EQ(m.bus.ram[0x50], 0x2A) << "a real, unmodified assembly should still run after QUIT + re-entry";
    EXPECT_EQ(out.find("?NO PROGRAM"), std::string::npos) << "got: " << out;
}

TEST(Editor, EndToEndTypeListAssembleAndRun) {
    // type, LIST, ASM (undefined label reports the real line number), fix, ASM,
    // RUN; the resume JMP lands in the shell
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    typeLine(m, "10 LDA #$2A");
    typeLine(m, "20 STA $50");
    typeLine(m, "30 JMP TARGET");   // undefined label on purpose, fixed below

    out.clear();
    typeLine(m, "LIST");
    EXPECT_NE(out.find("10         LDA #$2A"), std::string::npos) << "got: " << out;
    EXPECT_NE(out.find("20         STA $50"), std::string::npos) << "got: " << out;

    out.clear();
    typeLine(m, "ASM");
    EXPECT_NE(out.find("ERR LINE 30"), std::string::npos) << "expected the real line number in the error: got: " << out;

    // Fix it: an EDIT of line 30 with a real, defined target.
    typeLine(m, "5 TARGET: NOP");
    type(m, "EDIT 30\r");
    m.run_cycles(300000);
    typeLine(m, "JMP TARGET");

    out.clear();
    typeLine(m, "ASM");
    EXPECT_NE(out.find("Ok"), std::string::npos) << "got: " << out;

    // 5 TARGET: NOP -> $0400; 10 LDA #$2A -> $0401; 20 STA $50 -> $0403;
    // 30 JMP TARGET -> $0405.
    const uint8_t expected[] = { 0xEA, 0xA9, 0x2A, 0x85, 0x50, 0x4C, 0x00, 0x04 };
    for (size_t i = 0; i < sizeof(expected); i++) {
        EXPECT_EQ(m.bus.ram[kObjStart + i], expected[i]) << "byte " << i;
    }

    // RUN, step, and check $50 picked up the STA'd value
    out.clear();
    typeLine(m, "RUN");
    m.run_cycles(20000);
    EXPECT_EQ(m.bus.ram[0x50], 0x2A);
}

TEST(Editor, RunResumeLandsBackInTheShellNotRawWozmon) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    // resume convention: end with JMP SHELL_PROMPT
    typeLine(m, "10 JMP 8000");   // placeholder, replaced with SHELL_PROMPT via EDIT
    char buf[16];
    std::snprintf(buf, sizeof(buf), "JMP $%X", labelAddr("SHELL_PROMPT"));
    type(m, "EDIT 10\r");
    m.run_cycles(300000);
    typeLine(m, buf);

    out.clear();
    typeLine(m, "ASM");
    ASSERT_NE(out.find("Ok"), std::string::npos) << "got: " << out;

    out.clear();
    typeLine(m, "RUN");
    m.run_cycles(50000);
    // Landed back at the shell's own prompt, not Wozmon's "\".
    EXPECT_NE(out.find(">"), std::string::npos) << "got: " << out;
    EXPECT_EQ(out.find("\\"), std::string::npos) << "landed in raw Wozmon instead of the shell: got: " << out;
}

TEST(Editor, CtrlCBreaksAGenuinelyInfiniteLoopBackToTheShell) {
    // NMI_HANDLER redirects Ctrl-C ($03) to SHELL_PROMPT, the only way to break
    // a program that never polls READCHAR
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    // tight infinite loop, JMP to itself
    typeLine(m, "10 START: JMP START");

    out.clear();
    typeLine(m, "ASM");
    ASSERT_NE(out.find("Ok"), std::string::npos) << "got: " << out;

    out.clear();
    typeLine(m, "RUN");
    // typeLine's budget passed with no reprompt, so it's spinning
    EXPECT_EQ(out.find(">"), std::string::npos) << "shell reprompted without a break: got: " << out;
    // JMP START parks pc here at every instruction boundary
    EXPECT_EQ(m.cpu.pc, kObjStart) << "expected the CPU parked at the JMP, mid-loop";

    // Ctrl-C: NMI fires though the loop never polls the ACIA
    m.type_char(0x03);
    m.run_cycles(50000);

    EXPECT_NE(out.find(">"), std::string::npos) << "expected the shell prompt back after Ctrl-C: got: " << out;

    // shell still works afterward
    out.clear();
    typeLine(m, "LIST");
    EXPECT_NE(out.find("10 START:  JMP START"), std::string::npos) << "got: " << out;
}

TEST(Editor, CtrlCAtTheShellPromptDiscardsAnyPartialLineAndReprompts) {
    // Ctrl-C mid-typing breaks the same way while idle in READLINE_ECHO's poll loop
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    out.clear();
    type(m, "10 GARBAGE");
    m.type_char(0x03);
    m.run_cycles(50000);
    EXPECT_NE(out.find(">"), std::string::npos) << "expected a fresh prompt after Ctrl-C: got: " << out;

    // abandoned partial line was not stored
    out.clear();
    typeLine(m, "LIST");
    EXPECT_EQ(out.find("GARBAGE"), std::string::npos) << "the aborted partial line leaked into the program: got: " << out;

    typeLine(m, "10 LDA #$2A");
    out.clear();
    typeLine(m, "LIST");
    EXPECT_NE(out.find("10         LDA #$2A"), std::string::npos) << "got: " << out;
}

TEST(Editor, QuitReturnsToRawWozmonPrompt) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    out.clear();
    typeLine(m, "QUIT");
    EXPECT_NE(out.find("\\"), std::string::npos) << "got: " << out;
}

TEST(Editor, StoringManyLinesEventuallyReportsFull) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    // Short numbered lines until the 4K source region fills (STORE_LINE's SRC_END
    // guard). Comment-only filler, since a real mnemonic would hit the line-number
    // ceiling first.
    bool sawFull = false;
    for (int i = 1; i <= 150 && !sawFull; i++) {
        out.clear();
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%d ; AAAAAAAAAAAAAAAAAAAAAAAAAAAAA", i);
        typeLine(m, buf);
        if (out.find("FULL") != std::string::npos) sawFull = true;
    }
    EXPECT_TRUE(sawFull) << "never reported FULL";
}

TEST(Editor, FreeReportsSourceAndObjectUsageSeparately) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    // fresh entry: SHELL_START seeds ASMPC to OBJ_START
    out.clear();
    typeLine(m, "FREE");
    EXPECT_NE(out.find("PROGRAM: 0 USED, 4094 FREE"), std::string::npos) << "got: " << out;
    EXPECT_NE(out.find("EXEC:    0 USED, 10240 FREE"), std::string::npos) << "got: " << out;

    // storing a line moves PROGRAM only: 2-byte number + "LDA #$2A" + CR = 11 bytes
    typeLine(m, "10 LDA #$2A");
    out.clear();
    typeLine(m, "FREE");
    EXPECT_NE(out.find("PROGRAM: 11 USED, 4083 FREE"), std::string::npos) << "got: " << out;
    EXPECT_NE(out.find("EXEC:    0 USED, 10240 FREE"), std::string::npos) << "got: " << out;

    // ASM moves EXEC to the object size
    typeLine(m, "ASM");
    out.clear();
    typeLine(m, "FREE");
    EXPECT_NE(out.find("EXEC:    2 USED, 10238 FREE"), std::string::npos) << "got: " << out;

    // NEW clears object code too, so a stale RUN can't execute the old program
    typeLine(m, "NEW");
    out.clear();
    typeLine(m, "FREE");
    EXPECT_NE(out.find("PROGRAM: 0 USED, 4094 FREE"), std::string::npos) << "got: " << out;
    EXPECT_NE(out.find("EXEC:    0 USED, 10240 FREE"), std::string::npos) << "got: " << out;
}

} // namespace
