// GoogleTest suite for the resident command shell (cpu6502/rom/editor.s):
// the line-numbered program store (insert/replace/delete by number),
// the shell's own command dispatcher (NEW/LIST/EDIT/ASM/RUN/QUIT), and
// auto-numbering. Reached from Wozmon via a real typed "<addr>R" into
// SHELL_ENTRY -- everything after that is typed at the shell's own ">"
// prompt, exactly as a human would, entirely through the simulated
// ACIA/terminal.

#include <gtest/gtest.h>

#include "../machine.h"

#include <fstream>
#include <string>
#include <vector>

using namespace machine;

namespace {

// Real, built addresses -- verified against tmp/firmware.lbl after each
// `make -C ../../../cpu6502/rom` (see editor.s's header).
constexpr uint16_t kShellEntry = 0x8000;
constexpr uint16_t kObjStart = 0x0400;

// The ACIA has only a one-byte RX register -- pushing a second char before
// the NMI handler has drained the first (into SERIAL_BUFFER) overwrites
// it, a real overrun -- see machine_test.cpp's boot test for the same
// reasoning. Real per-character pacing throughout this file.
void type(Machine& m, const std::string& s) {
    for (char c : s) { m.type_char(uint8_t(c)); m.run_cycles(1000); }
}

// Post-CR budget: every printed character (including this ROM's own
// terse status/error/list output) goes through a real per-character ACIA
// delay (CHLL, bios.s, ~1275 cycles) before the byte even leaves CHROUT,
// so a multi-line reply needs real headroom, not just "long enough for
// the logic to run" -- 300000 cycles covers roughly 200+ output chars,
// comfortable for anything this file types short of the dedicated paged-
// LIST test (which budgets its own, larger allowance directly).
void typeLine(Machine& m, const std::string& s) {
    type(m, s);
    type(m, "\r");
    m.run_cycles(300000);
}

// Types "<addr>R\r" -- the real Wozmon run-command sequence SHELL_ENTRY
// is reached through.
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

// Boots and enters the shell -- the common starting point for every test
// below.
Machine bootIntoShell(std::string& out) {
    Machine m = boot(out);
    out.clear();
    runAt(m, kShellEntry);
    return m;
}

// Reads one label's real address straight out of the build's own
// tmp/firmware.lbl (ca65's "al <addr> .<name>" lines) -- unlike kShellEntry
// above, SHELL_PROMPT isn't a pinned/fixed address (only SHELL_ENTRY and
// the OS-call jump table are), so it genuinely drifts release to release
// as editor.s's own code shifts around; hardcoding it here just means
// re-breaking this test on every unrelated ROM change. This is the same
// value gen_entrypoints.py extracts for the browser side, read the same
// way.
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
    // CHECK_SYNTAX (editor.s's STORE_LINE) catches an unrecognized
    // mnemonic or malformed operand the instant a line is entered --
    // classic Microsoft BASIC's own immediate "?SYNTAX ERROR" on a bad
    // line, not a silent accept caught only later at ASM/RUN time. It
    // deliberately does NOT resolve labels (they aren't defined yet at
    // entry time, and checking them isn't the point): a forward reference
    // to a label that won't exist until a later line must be accepted
    // here exactly as real assemblers accept forward references, with
    // ASM's own two-pass label resolution the only place that would
    // ever legitimately reject an undefined one.
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
    // READCHAR (bios.s) already echoes the raw backspace byte itself --
    // a bare cursor move on a real terminal, not an erase -- so without
    // READLINE_ECHO's own fix, a deleted character's glyph would linger
    // on screen under whatever gets typed next (visible as leftover
    // "ghost" text, worst when retyping a shorter line over a longer
    // one). Checked on the raw echoed byte stream, not rendered text --
    // this is a terminal-control-sequence claim (what bytes went out),
    // not a content claim.
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    out.clear();
    type(m, "10 LDA");
    m.type_char(0x08);   // backspace over the trailing 'A'
    m.run_cycles(50000);

    // READCHAR's own bare BS echo, then READLINE_ECHO's added erase: a
    // space (overwrites the 'A'), then a second BS (backs over the space
    // too) -- net cursor position matches a plain non-destructive
    // backspace, but the glyph is actually gone.
    EXPECT_NE(out.find("\x08 \x08"), std::string::npos) << "got: " << out;
}

TEST(Editor, DelAlsoErasesDestructivelyAndDoesNotCorruptTheStoredLine) {
    // The real, practically important case: xterm.js's Backspace key (the
    // physical key labeled "Backspace" on any keyboard, browser terminal
    // via app.js) sends DEL ($7F), not BS ($08) -- confirmed by spying on
    // Machine.typeChar in the actual browser. Before this fix,
    // READLINE_ECHO only recognized $08, so every real backspace press in
    // the browser silently stored the invisible $7F byte straight into
    // LINE_BUF as an ordinary character instead of erasing anything -- a
    // real data-corruption bug (an invisible garbage byte embedded
    // mid-line), not just a cosmetic one, and the actual root cause of
    // the reported "ghost lines" in the program.
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    out.clear();
    type(m, "10 LDA");
    m.type_char(0x7F);   // DEL -- the real browser Backspace byte
    m.run_cycles(50000);

    // READCHAR's own echo of the raw $7F (a real terminal drops it as a
    // no-op when rendering, but the byte still goes out over CHROUT),
    // then READLINE_ECHO supplying the missing leading BS itself (unlike
    // $08, a bare $7F never moves a real terminal's cursor on its own),
    // then the shared erase (space, BS).
    EXPECT_NE(out.find("\x7F\x08 \x08"), std::string::npos) << "got: " << out;

    // And the corrected line stores and lists cleanly -- no stray $7F
    // leaked into the program the way it silently did before this fix.
    // LDX needs a real operand (there's no implied form) -- CHECK_SYNTAX
    // (editor.s's STORE_LINE) would otherwise correctly reject this as
    // bad syntax, same as it would for a human's own typo.
    type(m, "X #$05\r");   // finish the line as "10 LDX #$05"
    m.run_cycles(300000);
    out.clear();
    typeLine(m, "LIST");
    EXPECT_NE(out.find("10         LDX #$05"), std::string::npos) << "got: " << out;
    EXPECT_EQ(out.find("\x7F"), std::string::npos) << "a stray DEL byte leaked into the listing: got: " << out;
}

TEST(Editor, BackspaceOnAnEmptyLineIsANoOp) {
    // Y=0 -- nothing typed yet on this line -- must not touch LINE_BUF or
    // emit the destructive erase sequence for either backspace byte;
    // only READCHAR's own automatic echo of the raw byte happens
    // (pre-existing, unchanged behavior).
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

    // The shell is still genuinely usable afterward -- a real line still
    // stores and lists correctly, proving no stray state was left behind.
    out.clear();
    typeLine(m, "10 NOP");
    out.clear();
    typeLine(m, "LIST");
    EXPECT_NE(out.find("10         NOP"), std::string::npos) << "got: " << out;
}

TEST(Editor, ListFormatsLabelMnemonicOperandAndCommentIntoAlignedColumns) {
    // PRINT_ENTRY reformats the stored (free-form-typed) text on the fly
    // into fixed columns for LIST/EDIT display -- label field, mnemonic,
    // operand, and (if the line has one) a trailing ";" comment, all
    // realigned regardless of how many spaces the user actually typed.
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    typeLine(m, "10 START: LDA #$2A ; load the answer");
    typeLine(m, "20 STA $50");
    typeLine(m, "30 NOP");
    typeLine(m, "40 ; a full-line comment, no code at all");

    out.clear();
    typeLine(m, "LIST");
    // Label field: "START:" padded out to the same column every unlabeled
    // line's mnemonic starts at.
    EXPECT_NE(out.find("10 START:  LDA #$2A      ; LOAD THE ANSWER"), std::string::npos) << "got: " << out;
    EXPECT_NE(out.find("20         STA $50"), std::string::npos) << "got: " << out;
    EXPECT_NE(out.find("30         NOP"), std::string::npos) << "got: " << out;
    // A comment-only line: no mnemonic/operand, just the comment realigned
    // to the same column as every other line's own comment would be.
    EXPECT_NE(out.find("40                       ; A FULL-LINE COMMENT, NO CODE AT ALL"), std::string::npos) << "got: " << out;
}

TEST(Editor, ExplicitlyNumberedLinesListBackInSortedOrder) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    // Entered out of order -- LIST must still come back sorted by number.
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

    // 25 unnumbered lines -- auto-numbered 10, 20, ... 250 -- more than
    // one LIST_PAGE (20).
    for (int i = 0; i < 25; i++) typeLine(m, "NOP");

    out.clear();
    type(m, "LIST\r");
    // Column-formatted lines run ~17 chars apiece now (vs. the old raw
    // "N TEXT\r\n" dump), each still paced through CHLL's real per-char
    // ACIA delay -- budgeted generously for a full 20-line page.
    m.run_cycles(900000);
    EXPECT_NE(out.find("--MORE--"), std::string::npos) << "got tail: " << out.substr(out.size() > 200 ? out.size() - 200 : 0);
    // Not everything printed yet -- line 250 (the 25th) is still pending
    // behind the pause.
    EXPECT_EQ(out.find("250         NOP"), std::string::npos) << "printed past the page break: " << out;

    // Any keypress continues to the rest.
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

TEST(Editor, EndToEndTypeListAssembleAndRun) {
    // The plan's demo scenario, updated for the shell: type a program,
    // LIST, ASM (catching an undefined-label error with the *real* line
    // number), fix it, ASM again, RUN, and confirm RUN's resume JMP lands
    // back in the shell (SHELL_PROMPT), not raw Wozmon.
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

    // RUN it -- the loop (JMP TARGET at $0400) executes real object code;
    // step it a bit and confirm memory location $50 picked up the STA'd
    // value, proving RUN actually transferred control there.
    out.clear();
    typeLine(m, "RUN");
    m.run_cycles(20000);
    EXPECT_EQ(m.bus.ram[0x50], 0x2A);
}

TEST(Editor, RunResumeLandsBackInTheShellNotRawWozmon) {
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    // A program that ends with "JMP SHELL_PROMPT" (the documented resume
    // convention) instead of trapping on BRK.
    typeLine(m, "10 JMP 8000");   // placeholder -- overwritten with the real
                                  // SHELL_PROMPT address below via EDIT, so
                                  // this test doesn't hardcode it twice.
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
    // bios.s's NMI_HANDLER: Ctrl-C (ASCII ETX, $03) is recognized in the
    // receive-interrupt handler itself and redirected straight to
    // SHELL_PROMPT instead of RTI-ing back to the interrupted code -- the
    // only way to break a program that never polls READCHAR of its own
    // accord, since NMI fires regardless of what the CPU is doing.
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    // A tight, genuinely infinite loop -- JMP to its own address, nothing
    // else. No cooperative check of any kind would ever get a chance to
    // run here; only a real interrupt can break it.
    typeLine(m, "10 START: JMP START");

    out.clear();
    typeLine(m, "ASM");
    ASSERT_NE(out.find("Ok"), std::string::npos) << "got: " << out;

    out.clear();
    typeLine(m, "RUN");
    // typeLine's own post-CR budget (300000 cycles, ~100000 loop
    // iterations at 3 cycles each) already ran without ever reaching a
    // shell reprompt -- proof this is genuinely spinning, not just slow
    // to answer.
    EXPECT_EQ(out.find(">"), std::string::npos) << "shell reprompted without a break: got: " << out;
    // JMP START always resets pc to the same address every 3 cycles, so
    // at any instruction boundary the CPU is parked exactly here -- direct
    // proof it's mid-loop, not off in the weeds somewhere.
    EXPECT_EQ(m.cpu.pc, kObjStart) << "expected the CPU parked at the JMP, mid-loop";

    // Ctrl-C: NMI fires immediately even though the looping code never
    // once polls the ACIA.
    m.type_char(0x03);
    m.run_cycles(50000);

    EXPECT_NE(out.find(">"), std::string::npos) << "expected the shell prompt back after Ctrl-C: got: " << out;

    // And the shell is genuinely usable afterward, not just printing a
    // prompt over a stack left in a bad state -- a real command still
    // works.
    out.clear();
    typeLine(m, "LIST");
    EXPECT_NE(out.find("10 START:  JMP START"), std::string::npos) << "got: " << out;
}

TEST(Editor, CtrlCAtTheShellPromptDiscardsAnyPartialLineAndReprompts) {
    // Ctrl-C mid-typing, before Enter is ever pressed -- exercises the same
    // break path while idle in READLINE_ECHO's own poll loop, not a running
    // program. NMI_HANDLER doesn't distinguish the two cases; it always
    // abandons whatever was interrupted and lands back at a fresh prompt.
    SKIP_UNLESS_ROM_BUILT();
    std::string out;
    Machine m = bootIntoShell(out);

    out.clear();
    type(m, "10 GARBAGE");
    m.type_char(0x03);
    m.run_cycles(50000);
    EXPECT_NE(out.find(">"), std::string::npos) << "expected a fresh prompt after Ctrl-C: got: " << out;

    // The abandoned partial line never got stored -- LIST comes back with
    // no trace of it, not corrupted by half of what was mid-typing.
    out.clear();
    typeLine(m, "LIST");
    EXPECT_EQ(out.find("GARBAGE"), std::string::npos) << "the aborted partial line leaked into the program: got: " << out;

    // And the shell still works normally afterward.
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

    // Many distinct, explicitly-numbered short lines, well under any
    // single-line cap, until their combined total exhausts the 4K source
    // region -- the real scenario STORE_LINE's SRC_END guard protects
    // against. A comment-only line (CHECK_SYNTAX skips everything from
    // ';' on, same as ASM itself) is filler here -- any real mnemonic
    // repeated this many times would hit STORE_LINE's own line-number
    // ceiling long before the buffer itself filled up.
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

} // namespace
