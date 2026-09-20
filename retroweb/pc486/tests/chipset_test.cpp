// GoogleTest suite for this machine's glue-logic layer: memory read/write
// and ROM write-protection, the A20 gate's effect on wraparound vs.
// open-bus behavior, port I/O dispatch to the owned devices (including the
// new secondary IDE channel for the ATAPI CD-ROM), and the master/slave
// PIC cascade through poll_interrupt(). Adapted from
// ibmpc-at/tests/chipset_test.cpp -- unchanged in spirit, just this
// machine's own RAM size/floppy geometry, plus new CD-ROM coverage.

#include <gtest/gtest.h>

#include "chipset.h"

#include <vector>

namespace {

using pc486::Chipset;

TEST(ChipsetTest, MemoryReadWriteRoundTrip) {
    Chipset cs;
    cs.mem[0x1234] = 0;
    auto bus = cs.make_bus();
    bus.write(bus.ctx, 0x1234, 0xAB);
    EXPECT_EQ(bus.read(bus.ctx, 0x1234), 0xAB);
}

TEST(ChipsetTest, ExtendedMemoryAboveOneMebIsReadWriteWhenA20Open) {
    // This machine's 32MB (kRamSize) is genuinely installed, unlike the
    // AT's flat 1MB -- confirm the extended region actually round-trips
    // once the A20 gate is open, not just the legacy 1MB window.
    Chipset cs;
    auto bus = cs.make_bus();
    cs.kbc.out(0x64, 0xD1);
    cs.kbc.out(0x60, 0x02);  // bit1 set -> A20 enabled
    ASSERT_TRUE(cs.kbc.a20_enabled());
    bus.write(bus.ctx, 0x00200000, 0x55);  // 2MB mark, well above the legacy 1MB window
    EXPECT_EQ(bus.read(bus.ctx, 0x00200000), 0x55);
}

TEST(ChipsetTest, RomRegionRejectsWrites) {
    Chipset cs;
    uint8_t rom[4] = {0x11, 0x22, 0x33, 0x44};
    cs.load_rom(0xF0000, rom, 4);
    auto bus = cs.make_bus();
    EXPECT_EQ(bus.read(bus.ctx, 0xF0000), 0x11);
    bus.write(bus.ctx, 0xF0000, 0xFF);
    EXPECT_EQ(bus.read(bus.ctx, 0xF0000), 0x11);  // unchanged -- ROM ignores writes
}

TEST(ChipsetTest, A20DisabledWrapsAt1MB) {
    Chipset cs;
    auto bus = cs.make_bus();
    // A20 starts disabled (see i8042.h): address 0x100010 should alias 0x10.
    bus.write(bus.ctx, 0x000010, 0x77);
    EXPECT_EQ(bus.read(bus.ctx, 0x100010), 0x77);
}

TEST(ChipsetTest, A20EnabledDoesNotWrapAndReachesExtendedRam) {
    Chipset cs;
    auto bus = cs.make_bus();
    bus.write(bus.ctx, 0x000010, 0x77);
    cs.kbc.out(0x64, 0xD1);
    cs.kbc.out(0x60, 0x02);  // bit1 set -> A20 enabled
    ASSERT_TRUE(cs.kbc.a20_enabled());
    EXPECT_EQ(bus.read(bus.ctx, 0x100010), 0x00);  // no wraparound; genuinely-installed extended RAM, zero-initialized
    EXPECT_EQ(bus.read(bus.ctx, 0x000010), 0x77);  // low memory unaffected
}

// --- the page-resolution fast path (PC486_REVIEW.md §15) -----------------
// page_host() has to answer exactly what mem_read/mem_write would, for every
// byte of the page it hands over -- and refuse the page outright wherever
// they would do something other than touch `mem`.

TEST(ChipsetTest, PageHostHandsOverPlainRamAndRefusesDeviceOrRomPages) {
    Chipset cs;
    uint8_t rom[4] = {0x11, 0x22, 0x33, 0x44};
    cs.load_rom(0xF0000, rom, 4);
    EXPECT_EQ(cs.page_host(0x01000, false), cs.mem.data() + 0x01000);
    EXPECT_EQ(cs.page_host(0x01000, true), cs.mem.data() + 0x01000);
    EXPECT_EQ(cs.page_host(0xA0000, false), nullptr);  // VGA window: the card answers
    EXPECT_EQ(cs.page_host(0xBF000, true), nullptr);   // ... to its last page too
    EXPECT_EQ(cs.page_host(0xF0000, false), cs.mem.data() + 0xF0000);  // ROM reads are plain memory
    EXPECT_EQ(cs.page_host(0xF0000, true), nullptr);   // ... but writes must go the slow way
    cs.kbc.out(0x64, 0xD1);
    cs.kbc.out(0x60, 0xDF);  // A20 on, so an address above 1MB stays there
    EXPECT_EQ(cs.page_host(Chipset::kRamSize, false), nullptr);  // open bus above the RAM
}

TEST(ChipsetTest, PageHostFollowsTheA20GateAndBumpsItsEpochWhenTheGateMoves) {
    Chipset cs;
    const uint32_t before = *cs.map_epoch();
    // Gate closed: a page above 1MB aliases down, exactly as mem_read does.
    EXPECT_EQ(cs.page_host(0x100000, false), cs.mem.data());
    auto bus = cs.make_bus();
    bus.out(bus.ctx, 0x64, 0xD1);
    bus.out(bus.ctx, 0x60, 0xDF);  // bit1 set -> A20 enabled (and the reset line left high)
    ASSERT_TRUE(cs.kbc.a20_enabled());
    EXPECT_NE(*cs.map_epoch(), before);  // an already-resolved page is now wrong
    EXPECT_EQ(cs.page_host(0x100000, false), cs.mem.data() + 0x100000);
}

TEST(ChipsetTest, MapEpochAlsoMovesForARomLoadAndForAGateChangeMadeOutsideIoOut) {
    Chipset cs;
    uint8_t rom[4] = {0, 0, 0, 0};
    uint32_t e0 = *cs.map_epoch();
    cs.load_rom(0xC0000, rom, 4);
    EXPECT_NE(*cs.map_epoch(), e0);
    // A host poking the controller directly bypasses io_out; tick() runs
    // after every instruction and catches it anyway.
    e0 = *cs.map_epoch();
    cs.kbc.out(0x64, 0xD1);
    cs.kbc.out(0x60, 0xDF);
    cs.tick(1000, 1193182.0);
    EXPECT_NE(*cs.map_epoch(), e0);
}

TEST(ChipsetTest, Port61GatesSpeakerAndReadsBackRefreshToggle) {
    Chipset cs;
    auto bus = cs.make_bus();
    bus.out(bus.ctx, 0x61, 0x01);  // gate channel 2 on
    uint8_t before = bus.in(bus.ctx, 0x61);
    cs.tick(1000, 1193182.0);
    uint8_t after = bus.in(bus.ctx, 0x61);
    EXPECT_NE(before & 0x10, after & 0x10);  // refresh toggle flipped by tick()
    EXPECT_EQ(before & 0x01, 0x01);          // gate bit reads back as programmed
}

TEST(ChipsetTest, DmaPageRegisterRoundTripsThroughPorts) {
    Chipset cs;
    auto bus = cs.make_bus();
    bus.out(bus.ctx, 0x81, 0x05);  // DMA1 channel 2 (floppy) page register
    EXPECT_EQ(bus.in(bus.ctx, 0x81), 0x05);
}

TEST(ChipsetTest, MasterPicInterruptDeliveredDirectly) {
    Chipset cs;
    cs.pic_master.out(0x20, 0x11);
    cs.pic_master.out(0x21, 0x08);
    cs.pic_master.out(0x21, 0x04);
    cs.pic_master.out(0x21, 0x01);
    cs.pic_master.out(0x21, 0x00);  // unmask all
    cs.pic_master.raise(1);         // e.g. keyboard IRQ1
    EXPECT_EQ(cs.poll_interrupt(), 0x08 + 1);
}

TEST(ChipsetTest, FloppyDmaTransferCopiesRealBytesIntoMemory) {
    // 80 cyl / 2 head / 18 sec/track -- this machine's 1.44MB 3.5" geometry.
    Chipset cs;
    std::vector<uint8_t> img(80 * 2 * 18 * 512, 0);
    img[512] = 0x77;  // sector 2 (1-based), first byte
    cs.fdc.mount(0, img.data(), img.size());
    cs.fdc.out(0x3F2, 0x14);  // DOR: ~RESET high, motor A on, drive 0 select
    cs.dma1.out(0x0A, 0x02);  // unmask channel 2
    cs.dma1.out(0x04, 0x00); cs.dma1.out(0x04, 0x20);  // address = 0x2000
    cs.dma1.out(0x05, 0xFF); cs.dma1.out(0x05, 0x01);  // count = 0x01FF (512 bytes)

    // READ DATA: drive0/head0, C=0,H=0,R=2 (sector 2), N=2, EOT=2 (one sector).
    cs.fdc.out(0x3F5, 0xE6);
    cs.fdc.out(0x3F5, 0x00);
    cs.fdc.out(0x3F5, 0x00);
    cs.fdc.out(0x3F5, 0x00);
    cs.fdc.out(0x3F5, 0x02);
    cs.fdc.out(0x3F5, 0x02);
    cs.fdc.out(0x3F5, 0x02);
    cs.fdc.out(0x3F5, 0x1B);
    cs.fdc.out(0x3F5, 0xFF);

    bool copied = false;
    for (uint64_t c = 0; c < 10'000'000 && !copied; c += 100) {
        cs.tick(c, 66000000.0);
        if (cs.mem[0x2000] == 0x77) copied = true;
    }
    EXPECT_TRUE(copied);
    EXPECT_EQ(cs.dma1.address(2), 0x2000 + 512);
    EXPECT_TRUE(cs.fdc.irq_pending());
}

TEST(ChipsetTest, Irq6IsEdgeTriggeredNotReRaisedWhileStillPending) {
    Chipset cs;
    cs.pic_master.out(0x20, 0x11);
    cs.pic_master.out(0x21, 0x08);
    cs.pic_master.out(0x21, 0x04);
    cs.pic_master.out(0x21, 0x01);
    cs.pic_master.out(0x21, 0x00);  // unmask all, vector base 8

    std::vector<uint8_t> img(80 * 2 * 18 * 512, 0);
    cs.fdc.mount(0, img.data(), img.size());
    cs.fdc.out(0x3F2, 0x1C);  // DOR: ~RESET high, motor A on, DMA/IRQ enable
    cs.dma1.out(0x0A, 0x02);
    cs.dma1.out(0x04, 0x00); cs.dma1.out(0x04, 0x20);
    cs.dma1.out(0x05, 0xFF); cs.dma1.out(0x05, 0x01);
    cs.fdc.out(0x3F5, 0xE6);
    cs.fdc.out(0x3F5, 0x00); cs.fdc.out(0x3F5, 0x00); cs.fdc.out(0x3F5, 0x00);
    cs.fdc.out(0x3F5, 0x01); cs.fdc.out(0x3F5, 0x02); cs.fdc.out(0x3F5, 0x01);
    cs.fdc.out(0x3F5, 0x1B); cs.fdc.out(0x3F5, 0xFF);

    for (uint64_t c = 0; c < 10'000'000; c += 100) {
        cs.tick(c, 66000000.0);
        if (cs.fdc.irq_pending()) break;
    }
    ASSERT_TRUE(cs.fdc.irq_pending());

    int vec1 = cs.poll_interrupt();
    ASSERT_GE(vec1, 0);
    cs.pic_master.out(0x20, 0x20);  // non-specific EOI, as the ISR would issue
    EXPECT_TRUE(cs.fdc.irq_pending());  // still not drained
    EXPECT_FALSE(cs.pic_master.has_interrupt());  // but no new edge -> nothing re-pending

    cs.tick(20'000'000, 66000000.0);
    EXPECT_FALSE(cs.pic_master.has_interrupt());
}

TEST(ChipsetTest, KeyboardIrq1ReachesThePic) {
    Chipset cs;
    cs.pic_master.out(0x20, 0x11);
    cs.pic_master.out(0x21, 0x08);
    cs.pic_master.out(0x21, 0x04);
    cs.pic_master.out(0x21, 0x01);
    cs.pic_master.out(0x21, 0x00);  // unmask all, vector base 8
    cs.kbc.out(0x64, 0x60); cs.kbc.out(0x60, 0x01);  // command byte: enable IRQ1
    cs.kbc.in(0x60);  // drain the keyboard's power-on BAT byte, as BIOS POST does

    cs.kbc.inject_scancode(0x1E);  // 'A' make code
    ASSERT_TRUE(cs.kbc.irq1_pending());
    cs.tick(0, 66000000.0);
    EXPECT_TRUE(cs.pic_master.has_interrupt());
    EXPECT_EQ(cs.poll_interrupt(), 0x08 + 1);  // vector_base(8) + IR1
}

TEST(ChipsetTest, CdromOwnsSecondaryChannelWithoutStealingHddsPrimaryPorts) {
    // The secondary IDE channel (CD-ROM) and primary channel (HDD) must
    // each own only their own ports -- a too-wide range on either would
    // silently steal the other's registers, the exact class of bug
    // fdc765/wd1003's own 0x3F6-ownership tests already guard against on
    // this machine's inherited devices.
    Chipset cs;
    EXPECT_TRUE(cs.cdrom.owns(0x170));
    EXPECT_TRUE(cs.cdrom.owns(0x177));
    EXPECT_TRUE(cs.cdrom.owns(0x376));
    EXPECT_FALSE(cs.cdrom.owns(0x1F0));  // the HDD's primary-channel data register
    EXPECT_FALSE(cs.hdd.owns(0x170));    // and not the other way around
}

TEST(ChipsetTest, CdromDataRegisterGoesThroughAtomicSixteenBitBusPath) {
    // Same atomic-16-bit-access need as the HDD's 0x1F0 (see
    // cpu80486.h/chipset.h's in16/out16 comments) -- IDENTIFY PACKET DEVICE
    // (0xA1) is the standard ATAPI probe issued via the command register
    // (0x177), and its result must be readable as real 16-bit words at
    // 0x170, not decomposed into two 8-bit accesses that would misread an
    // adjacent, unrelated register as the "high byte".
    Chipset cs;
    auto bus = cs.make_bus();
    bus.out(bus.ctx, 0x176, 0xA0);  // drive/head: device 0 select
    bus.out(bus.ctx, 0x177, 0xA1);  // IDENTIFY PACKET DEVICE
    ASSERT_TRUE(bus.in(bus.ctx, 0x177) & 0x08);  // DRQ: data ready
    uint16_t word0 = bus.in16(bus.ctx, 0x170);
    // Word 0 bits 15-14 = 10b for an ATAPI packet device (ATA/ATAPI-4+
    // general-configuration convention) -- confirms this is genuinely the
    // 16-bit-wide identify buffer, not two stitched-together 8-bit reads
    // of whatever else happens to be at 0x170/0x171.
    EXPECT_EQ(word0 & 0xC000, 0x8000);
}

TEST(ChipsetTest, Irq15IsEdgeTriggeredOnSlaveLineSeven) {
    // Same edge-triggering discipline as IRQ6/IRQ1 above, for IRQ15 (the
    // CD-ROM's secondary-channel interrupt), which lives on the slave
    // PIC's line 7 -- the standard secondary-IDE-channel IRQ assignment.
    Chipset cs;
    cs.pic_master.out(0x20, 0x11);
    cs.pic_master.out(0x21, 0x08);
    cs.pic_master.out(0x21, 0x04);
    cs.pic_master.out(0x21, 0x01);
    cs.pic_master.out(0x21, 0x00);  // unmask all, vector base 8
    cs.pic_slave.out(0xA0, 0x11);
    cs.pic_slave.out(0xA1, 0x70);
    cs.pic_slave.out(0xA1, 0x02);
    cs.pic_slave.out(0xA1, 0x01);
    cs.pic_slave.out(0xA1, 0x00);  // unmask all, vector base 0x70

    cs.cdrom.out(0x176, 0xA0);
    cs.cdrom.out(0x177, 0xA1);  // IDENTIFY PACKET DEVICE -- a command that completes and raises IRQ15

    bool seen = false;
    for (uint64_t c = 0; c < 10'000'000; c += 100) {
        cs.tick(c, 66000000.0);
        if (cs.cdrom.irq_pending()) { seen = true; break; }
    }
    ASSERT_TRUE(seen);

    int vec1 = cs.poll_interrupt();
    ASSERT_GE(vec1, 0);
    cs.pic_slave.out(0xA0, 0x20);   // non-specific EOI on the slave
    cs.pic_master.out(0x20, 0x20);  // ...and the cascaded master EOI
    EXPECT_FALSE(cs.pic_slave.has_interrupt());  // no new edge -> nothing re-pending

    cs.tick(20'000'000, 66000000.0);
    EXPECT_FALSE(cs.pic_slave.has_interrupt());
}

// Brings both PICs up the way a real AT BIOS POST does, with everything
// unmasked: master vector base 0x08 with a slave on IR2, slave base 0x70.
void InitPics(Chipset &cs) {
    cs.pic_master.out(0x20, 0x11);
    cs.pic_master.out(0x21, 0x08);
    cs.pic_master.out(0x21, 0x04);
    cs.pic_master.out(0x21, 0x01);
    cs.pic_master.out(0x21, 0x00);
    cs.pic_slave.out(0xA0, 0x11);
    cs.pic_slave.out(0xA1, 0x70);
    cs.pic_slave.out(0xA1, 0x02);
    cs.pic_slave.out(0xA1, 0x01);
    cs.pic_slave.out(0xA1, 0x00);
}

TEST(ChipsetTest, MousePacketBytesArriveOneInterruptAtATimeInOrder) {
    // A 3-byte AUX packet raises IRQ12 once per byte, and the controller
    // re-asserts the line inside the same in(0x60) that cleared it. A
    // handler that runs with interrupts enabled -- the firmware's INT 74h
    // does -- must therefore be protected by the PIC's in-service block, or
    // it re-enters itself between reading a byte and storing it and the
    // packet comes out in the wrong order (PC486_REVIEW.md §13).
    Chipset cs;
    InitPics(cs);
    cs.kbc.out(0x64, 0x60);  // write command byte: IRQ1 + IRQ12 on, AUX clock enabled
    cs.kbc.out(0x60, 0x03);
    cs.kbc.out(0x64, 0xD4);  // enable data reporting on the mouse itself
    cs.kbc.out(0x60, 0xF4);
    while (cs.kbc.in(0x64) & 0x01) cs.kbc.in(0x60);  // drain the ACK

    cs.inject_mouse_event(5, -3, 0x01);  // right and toward the user, left button down

    const uint8_t expect[3] = {0x08 | 0x01 | 0x20, 5, uint8_t(-3)};
    uint64_t cycles = 0;
    for (int byte = 0; byte < 3; ++byte) {
        // One interrupt per byte, and nothing else may be delivered while
        // this one is in service.
        int vec = -1;
        for (int i = 0; i < 64 && vec < 0; ++i) { cs.tick(cycles += 8, 66000000.0); vec = cs.poll_interrupt(); }
        ASSERT_EQ(vec, 0x74) << "byte " << byte << " did not raise IRQ12";
        EXPECT_EQ(cs.poll_interrupt(), -1) << "IRQ12 re-entered its own handler";
        ASSERT_EQ(cs.kbc.in(0x64) & 0x21, 0x21) << "byte " << byte << " is not tagged AUXB";
        EXPECT_EQ(cs.kbc.in(0x60), expect[byte]);
        cs.tick(cycles += 8, 66000000.0);
        EXPECT_EQ(cs.poll_interrupt(), -1) << "still blocked until EOI";
        cs.pic_slave.out(0xA0, 0x20);
        cs.pic_master.out(0x20, 0x20);
    }
    // Exactly three bytes: no fourth interrupt from the same packet.
    for (int i = 0; i < 64; ++i) cs.tick(cycles += 8, 66000000.0);
    EXPECT_EQ(cs.poll_interrupt(), -1);
}

TEST(ChipsetTest, SoundBlasterPlaybackDmaReadsMemoryIntoTheCard) {
    // Direction is the whole test. A D/A transfer moves memory *into* the
    // card; moving the card's own buffer out to memory instead leaves the
    // DAC playing whatever the card was holding and silently overwrites the
    // program's mixed audio (PC486_REVIEW.md §13).
    Chipset cs;
    const uint32_t phys = 0x00012000;
    const uint8_t pcm[8] = {0x80, 0xFF, 0x00, 0x80, 0xC0, 0x40, 0x80, 0x80};
    for (uint32_t i = 0; i < 8; ++i) cs.mem_write(phys + i, pcm[i]);

    // DMA1 channel 1: clear the flip-flop, program address/count, mode
    // 0x49 (single transfer, address increment, read-from-memory), unmask.
    cs.io_out(0x0C, 0x00);
    cs.io_out(0x02, uint8_t(phys & 0xFF));
    cs.io_out(0x02, uint8_t((phys >> 8) & 0xFF));
    cs.io_out(0x0C, 0x00);
    cs.io_out(0x03, 0x07);  // count = 8 - 1
    cs.io_out(0x03, 0x00);
    cs.io_out(0x83, uint8_t(phys >> 16));
    cs.io_out(0x0B, 0x49);
    cs.io_out(0x0A, 0x01);  // unmask channel 1

    cs.io_out(0x226, 1);
    cs.io_out(0x226, 0);
    ASSERT_EQ(cs.io_in(0x22A), 0xAA);
    cs.io_out(0x22C, 0xD1);                              // speaker on
    cs.io_out(0x22C, 0x40); cs.io_out(0x22C, 166);       // ~11 kHz time constant
    cs.io_out(0x22C, 0x14); cs.io_out(0x22C, 0x07); cs.io_out(0x22C, 0x00);

    for (uint64_t c = 0; c < 2'000'000 && cs.sb.playing(); c += 64) cs.tick(c, 66000000.0);
    EXPECT_FALSE(cs.sb.playing());

    auto samples = cs.sb.drain_samples();
    ASSERT_EQ(samples.size(), 8u);
    EXPECT_EQ(samples[0].left, 0);        // 80h unsigned is silence
    EXPECT_EQ(samples[1].left, 0x7F00);   // FFh
    EXPECT_EQ(samples[2].left, -32768);   // 00h
    EXPECT_EQ(samples[4].left, 0x4000);   // C0h
    // Playback reads memory; it must not write a byte of it.
    for (uint32_t i = 0; i < 8; ++i) EXPECT_EQ(cs.mem_read(phys + i), pcm[i]) << "byte " << i;
}

TEST(ChipsetTest, SoundBlasterRecordingDmaWritesTheCardsSamplesIntoMemory) {
    // The mirror image: an A/D transfer carries the card's own samples out to
    // memory. Nothing is plugged into the inputs, so what lands there is
    // unsigned-8-bit silence.
    Chipset cs;
    const uint32_t phys = 0x00013000;
    for (uint32_t i = 0; i < 4; ++i) cs.mem_write(phys + i, 0x5A);

    cs.io_out(0x0C, 0x00);
    cs.io_out(0x02, uint8_t(phys & 0xFF));
    cs.io_out(0x02, uint8_t((phys >> 8) & 0xFF));
    cs.io_out(0x0C, 0x00);
    cs.io_out(0x03, 0x03);  // count = 4 - 1
    cs.io_out(0x03, 0x00);
    cs.io_out(0x83, uint8_t(phys >> 16));
    cs.io_out(0x0B, 0x45);  // single transfer, write-to-memory
    cs.io_out(0x0A, 0x01);

    cs.io_out(0x226, 1);
    cs.io_out(0x226, 0);
    ASSERT_EQ(cs.io_in(0x22A), 0xAA);
    cs.io_out(0x22C, 0x40); cs.io_out(0x22C, 166);
    cs.io_out(0x22C, 0x24); cs.io_out(0x22C, 0x03); cs.io_out(0x22C, 0x00);
    EXPECT_TRUE(cs.sb.transfer_is_input());

    for (uint64_t c = 0; c < 2'000'000 && cs.sb.playing(); c += 64) cs.tick(c, 66000000.0);
    for (uint32_t i = 0; i < 4; ++i) EXPECT_EQ(cs.mem_read(phys + i), 0x80) << "byte " << i;
    EXPECT_TRUE(cs.sb.drain_samples().empty());  // recording produces no DAC output
}

TEST(ChipsetTest, SoundBlasterBlockEndRaisesIrq5) {
    Chipset cs;
    InitPics(cs);
    const uint32_t phys = 0x00014000;
    cs.io_out(0x0C, 0x00);
    cs.io_out(0x02, 0x00);
    cs.io_out(0x02, 0x40);
    cs.io_out(0x0C, 0x00);
    cs.io_out(0x03, 0x03);
    cs.io_out(0x03, 0x00);
    cs.io_out(0x83, uint8_t(phys >> 16));
    cs.io_out(0x0B, 0x49);
    cs.io_out(0x0A, 0x01);

    cs.io_out(0x226, 1);
    cs.io_out(0x226, 0);
    ASSERT_EQ(cs.io_in(0x22A), 0xAA);
    cs.io_out(0x22C, 0x40); cs.io_out(0x22C, 166);
    cs.io_out(0x22C, 0x14); cs.io_out(0x22C, 0x03); cs.io_out(0x22C, 0x00);

    int vec = -1;
    for (uint64_t c = 0; c < 2'000'000 && vec < 0; c += 64) { cs.tick(c, 66000000.0); vec = cs.poll_interrupt(); }
    EXPECT_EQ(vec, 0x0D);  // master vector base 8 + IR5
    cs.io_in(0x22E);       // acknowledge the 8-bit interrupt at base+Eh
    EXPECT_FALSE(cs.sb.irq_pending());
}

TEST(ChipsetTest, SlaveInterruptCascadesThroughMasterIr2) {
    Chipset cs;
    cs.pic_master.out(0x20, 0x11);
    cs.pic_master.out(0x21, 0x08);
    cs.pic_master.out(0x21, 0x04);
    cs.pic_master.out(0x21, 0x01);
    cs.pic_master.out(0x21, 0x00);
    cs.pic_slave.out(0xA0, 0x11);
    cs.pic_slave.out(0xA1, 0x70);  // vector base 0x70 (IRQ8-15 range)
    cs.pic_slave.out(0xA1, 0x02);
    cs.pic_slave.out(0xA1, 0x01);
    cs.pic_slave.out(0xA1, 0x00);
    cs.pic_slave.raise(0);  // e.g. IRQ8 (RTC)
    EXPECT_EQ(cs.poll_interrupt(), 0x70);
}

}  // namespace
