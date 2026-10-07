// glue logic: memory and ROM protection, A20 wrap, port dispatch, secondary IDE channel, PIC cascade

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
    // 32MB is installed; the extended region must round-trip once A20 is open
    Chipset cs;
    auto bus = cs.make_bus();
    cs.kbc.out(0x64, 0xD1);
    cs.kbc.out(0x60, 0x02);
    ASSERT_TRUE(cs.kbc.a20_enabled());
    bus.write(bus.ctx, 0x00200000, 0x55);
    EXPECT_EQ(bus.read(bus.ctx, 0x00200000), 0x55);
}

TEST(ChipsetTest, RomRegionRejectsWrites) {
    Chipset cs;
    uint8_t rom[4] = {0x11, 0x22, 0x33, 0x44};
    cs.load_rom(0xF0000, rom, 4);
    auto bus = cs.make_bus();
    EXPECT_EQ(bus.read(bus.ctx, 0xF0000), 0x11);
    bus.write(bus.ctx, 0xF0000, 0xFF);
    EXPECT_EQ(bus.read(bus.ctx, 0xF0000), 0x11);  // ROM ignores writes
}

TEST(ChipsetTest, TheBiosRomAnswersAgainAtTheTopOfFourGigabytes) {
    // first fetch after RESET (FFFFFFF0h)
    Chipset cs;
    uint8_t rom[4] = {0xEA, 0x5B, 0xE0, 0x00};
    cs.load_rom(0xFFFF0, rom, 4);
    cs.kbc.out(0x64, 0xD1);
    cs.kbc.out(0x60, 0x02);  // as an OS that reboots leaves it
    auto bus = cs.make_bus();
    EXPECT_EQ(bus.read(bus.ctx, 0xFFFFFFF0u), 0xEA);
    EXPECT_EQ(bus.read(bus.ctx, 0xFFFFFFF3u), 0x00);
    EXPECT_EQ(cs.page_host(0xFFFFF000u, false), cs.mem.data() + 0xFF000);
    bus.write(bus.ctx, 0xFFFFFFF0u, 0x90);
    EXPECT_EQ(bus.read(bus.ctx, 0xFFFF0), 0xEA) << "still ROM through the alias";
    EXPECT_EQ(bus.read(bus.ctx, 0xFFDFFFF0u), 0xFF) << "nothing answers below the top 2MB";
}

TEST(ChipsetTest, A20DisabledWrapsAt1MB) {
    Chipset cs;
    auto bus = cs.make_bus();
    // A20 starts disabled (i8042.h): 0x100010 aliases 0x10
    bus.write(bus.ctx, 0x000010, 0x77);
    EXPECT_EQ(bus.read(bus.ctx, 0x100010), 0x77);
}

TEST(ChipsetTest, A20EnabledDoesNotWrapAndReachesExtendedRam) {
    Chipset cs;
    auto bus = cs.make_bus();
    bus.write(bus.ctx, 0x000010, 0x77);
    cs.kbc.out(0x64, 0xD1);
    cs.kbc.out(0x60, 0x02);
    ASSERT_TRUE(cs.kbc.a20_enabled());
    EXPECT_EQ(bus.read(bus.ctx, 0x100010), 0x00);  // no wraparound
    EXPECT_EQ(bus.read(bus.ctx, 0x000010), 0x77);  // low memory unaffected
}

// --- page_host() fast path ---
// page_host() must answer exactly as mem_read/mem_write would, and refuse any page they would not serve from mem

TEST(ChipsetTest, PageHostHandsOverPlainRamAndRefusesDeviceOrRomPages) {
    Chipset cs;
    uint8_t rom[4] = {0x11, 0x22, 0x33, 0x44};
    cs.load_rom(0xF0000, rom, 4);
    EXPECT_EQ(cs.page_host(0x01000, false), cs.mem.data() + 0x01000);
    EXPECT_EQ(cs.page_host(0x01000, true), cs.mem.data() + 0x01000);
    EXPECT_EQ(cs.page_host(0xA0000, false), nullptr);  // VGA window
    EXPECT_EQ(cs.page_host(0xBF000, true), nullptr);
    EXPECT_EQ(cs.page_host(0xF0000, false), cs.mem.data() + 0xF0000);  // ROM reads are plain memory
    EXPECT_EQ(cs.page_host(0xF0000, true), nullptr);  // writes take the slow path
    cs.kbc.out(0x64, 0xD1);
    cs.kbc.out(0x60, 0xDF);
    EXPECT_EQ(cs.page_host(Chipset::kRamSize, false), nullptr);  // open bus above RAM
}

TEST(ChipsetTest, PageHostFollowsTheA20GateAndBumpsItsEpochWhenTheGateMoves) {
    Chipset cs;
    const uint32_t before = *cs.map_epoch();
    // gate closed: a page above 1MB aliases down
    EXPECT_EQ(cs.page_host(0x100000, false), cs.mem.data());
    auto bus = cs.make_bus();
    bus.out(bus.ctx, 0x64, 0xD1);
    bus.out(bus.ctx, 0x60, 0xDF);  // A20 on, reset line high
    ASSERT_TRUE(cs.kbc.a20_enabled());
    EXPECT_NE(*cs.map_epoch(), before);  // resolved pages are now stale
    EXPECT_EQ(cs.page_host(0x100000, false), cs.mem.data() + 0x100000);
}

TEST(ChipsetTest, MapEpochAlsoMovesForARomLoadAndForAGateChangeMadeOutsideIoOut) {
    Chipset cs;
    uint8_t rom[4] = {0, 0, 0, 0};
    uint32_t e0 = *cs.map_epoch();
    cs.load_rom(0xC0000, rom, 4);
    EXPECT_NE(*cs.map_epoch(), e0);
    // a host poke bypasses io_out; tick() catches it
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

TEST(ChipsetTest, EveryDmaPageRegisterPortReachesItsOwnChannel) {
    // AT page-register map (IBM AT Technical Reference): 87/83/81/82 for channels 0-3, 8F/8B/89/8A for 4-7
    struct Port { uint16_t port; int controller; int channel; };
    const Port ports[] = {
        {0x87, 1, 0}, {0x83, 1, 1}, {0x81, 1, 2}, {0x82, 1, 3},
        {0x8F, 2, 0}, {0x8B, 2, 1}, {0x89, 2, 2}, {0x8A, 2, 3},
    };
    Chipset cs;
    auto bus = cs.make_bus();
    uint8_t v = 0x10;
    for (const Port &p : ports) {
        bus.out(bus.ctx, p.port, v);
        EXPECT_EQ((p.controller == 1 ? cs.dma1 : cs.dma2).page(p.channel), v) << "port " << p.port;
        EXPECT_EQ(bus.in(bus.ctx, p.port), v);
        ++v;
    }
}

TEST(ChipsetTest, AWordOutToByteWidePortsSplitsLowByteFirst) {
    Chipset cs;
    auto bus = cs.make_bus();
    bus.out16(bus.ctx, 0x81, 0x0A05);  // page registers for DMA channels 2 and 3
    EXPECT_EQ(cs.dma1.page(2), 0x05);
    EXPECT_EQ(cs.dma1.page(3), 0x0A);
    EXPECT_EQ(bus.in16(bus.ctx, 0x81), 0x0A05);
}

TEST(ChipsetTest, TheVbeIndexAndDataPortsTakeAWholeWord) {
    // two byte writes would put the high byte on the data port
    Chipset cs;
    auto bus = cs.make_bus();
    bus.out16(bus.ctx, pc486::Ega::kVbeIndexPort, 0);   // VBE_DISPI_INDEX_ID
    bus.out16(bus.ctx, pc486::Ega::kVbeDataPort, 0xB0C4);
    EXPECT_EQ(bus.in16(bus.ctx, pc486::Ega::kVbeDataPort), 0xB0C4);
}

TEST(ChipsetTest, TheRtcPeriodicInterruptArrivesOnIrq8) {
    Chipset cs;
    cs.io_out(0x20, 0x11); cs.io_out(0x21, 0x08); cs.io_out(0x21, 0x04); cs.io_out(0x21, 0x01);
    cs.io_out(0x21, 0xFB);  // master: only the cascade
    cs.io_out(0xA0, 0x11); cs.io_out(0xA1, 0x70); cs.io_out(0xA1, 0x02); cs.io_out(0xA1, 0x01);
    cs.io_out(0xA1, 0xFE);  // slave: only IRQ8
    cs.io_out(0x70, 0x0B);
    cs.io_out(0x71, 0x42);  // PIE, 24-hour
    cs.tick(70000, 66000000.0);  // past one 1024 Hz period
    EXPECT_EQ(cs.poll_interrupt(), 0x70);
    cs.io_out(0x70, 0x0C);
    EXPECT_EQ(cs.io_in(0x71) & 0xC0, 0xC0) << "IRQF and PF, cleared by this read";
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
    // 1.44MB geometry
    Chipset cs;
    std::vector<uint8_t> img(80 * 2 * 18 * 512, 0);
    img[512] = 0x77;  // sector 2 (1-based), first byte
    cs.fdc.mount(0, img.data(), img.size());
    cs.fdc.out(0x3F2, 0x14);  // DOR: ~RESET high, motor A on, drive 0 select
    cs.dma1.out(0x0A, 0x02);  // unmask channel 2
    cs.dma1.out(0x04, 0x00); cs.dma1.out(0x04, 0x20);  // address = 0x2000
    cs.dma1.out(0x05, 0xFF); cs.dma1.out(0x05, 0x01);  // count = 0x01FF (512 bytes)

    // READ DATA, sector 2, one sector
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

TEST(ChipsetTest, FloppyDmaHoldsTheBusAndSnoopsTheL1) {
    Chipset cs;
    pc486::Cache486 cache;
    uint64_t clock = 0;
    cs.timing = &cache;
    cs.timing_clock = &clock;
    cache.read(0x2000, 4, true, 0);
    ASSERT_TRUE(cache.l1_has(0x2000));
    std::vector<uint8_t> img(80 * 2 * 18 * 512, 0);
    img[512] = 0x77;
    cs.fdc.mount(0, img.data(), img.size());
    cs.fdc.out(0x3F2, 0x14);
    cs.dma1.out(0x0A, 0x02);
    cs.dma1.out(0x04, 0x00); cs.dma1.out(0x04, 0x20);
    cs.dma1.out(0x05, 0xFF); cs.dma1.out(0x05, 0x01);
    for (uint8_t b : {0xE6, 0x00, 0x00, 0x00, 0x02, 0x02, 0x02, 0x1B, 0xFF}) cs.fdc.out(0x3F5, b);
    for (clock = 0; clock < 10'000'000 && cs.mem[0x2000] != 0x77; clock += 100) cs.tick(clock, 66000000.0);
    ASSERT_EQ(cs.mem[0x2000], 0x77);
    EXPECT_FALSE(cache.l1_has(0x2000)) << "the transfer into memory invalidated the line";
    // 512 single transfers of 48 bus clocks each hold the bus from the tick
    // that ran them, 100 clocks back; an L2 hit then waits for the rest.
    EXPECT_EQ(cache.read(0x2000, 4, true, clock), 512 * 48 * 2 - 100 + 2 * 2);
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
    cs.kbc.in(0x60);  // drain BAT

    cs.kbc.inject_scancode(0x1E);  // 'A' make code
    ASSERT_TRUE(cs.kbc.irq1_pending());
    cs.tick(0, 66000000.0);
    EXPECT_TRUE(cs.pic_master.has_interrupt());
    EXPECT_EQ(cs.poll_interrupt(), 0x08 + 1);  // vector_base(8) + IR1
}

TEST(ChipsetTest, SecondQueuedKeyboardByteStillReachesThePic) {
    // two keyboard bytes close together: in(0x60) refills the output register and re-asserts irq1 inside
    // the same call, so no 1->0->1 edge shows at tick granularity (same shape as the IRQ12 fix in chipset.cpp tick())
    Chipset cs;
    cs.pic_master.out(0x20, 0x11);
    cs.pic_master.out(0x21, 0x08);
    cs.pic_master.out(0x21, 0x04);
    cs.pic_master.out(0x21, 0x01);
    cs.pic_master.out(0x21, 0x00);  // unmask all, vector base 8
    cs.kbc.out(0x64, 0x60); cs.kbc.out(0x60, 0x01);  // command byte: enable IRQ1
    cs.kbc.in(0x60);  // drain BAT

    cs.kbc.inject_scancode(0x1E);        // 'A' make code
    cs.kbc.inject_scancode(0x9E);  // queued behind it
    cs.tick(0, 66000000.0);
    ASSERT_TRUE(cs.pic_master.has_interrupt());
    EXPECT_EQ(cs.poll_interrupt(), 0x08 + 1);  // service byte one
    // through io_in(), as a guest IN AL,60h does; that reopens tick()'s service gate
    EXPECT_EQ(cs.io_in(0x60), 0x1E);            // the ISR's own IN AL,60h
    cs.pic_master.out(0x20, 0x20);              // and its EOI

    // byte two is already in the output register
    ASSERT_TRUE(cs.kbc.irq1_pending());
    cs.tick(0, 66000000.0);
    EXPECT_TRUE(cs.pic_master.has_interrupt())
        << "byte two's interrupt never reached the PIC -- a purely "
           "interrupt-driven guest will never read it";
    EXPECT_EQ(cs.poll_interrupt(), 0x08 + 1);
    EXPECT_EQ(cs.kbc.in(0x60), 0x9E);
}

TEST(ChipsetTest, CdromOwnsSecondaryChannelWithoutStealingHddsPrimaryPorts) {
    // each IDE channel owns only its own ports
    Chipset cs;
    EXPECT_TRUE(cs.cdrom.owns(0x170));
    EXPECT_TRUE(cs.cdrom.owns(0x177));
    EXPECT_TRUE(cs.cdrom.owns(0x376));
    EXPECT_FALSE(cs.cdrom.owns(0x1F0));  // the HDD's primary-channel data register
    EXPECT_FALSE(cs.hdd.owns(0x170));    // and not the other way around
}

TEST(ChipsetTest, CdromDataRegisterGoesThroughAtomicSixteenBitBusPath) {
    // IDENTIFY PACKET DEVICE (0xA1) must read as real 16-bit words at 0x170
    Chipset cs;
    auto bus = cs.make_bus();
    bus.out(bus.ctx, 0x176, 0xA0);  // drive/head: device 0 select
    bus.out(bus.ctx, 0x177, 0xA1);  // IDENTIFY PACKET DEVICE
    ASSERT_TRUE(bus.in(bus.ctx, 0x177) & 0x08);  // DRQ: data ready
    uint16_t word0 = bus.in16(bus.ctx, 0x170);
    // word 0 bits 15-14 = 10b marks an ATAPI packet device
    EXPECT_EQ(word0 & 0xC000, 0x8000);
}

TEST(ChipsetTest, Irq15IsEdgeTriggeredOnSlaveLineSeven) {
    // edge-triggering for IRQ15, slave PIC line 7 (secondary IDE)
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
    cs.cdrom.out(0x177, 0xA1);  // raises IRQ15

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

// both PICs up as AT BIOS POST leaves them: master base 0x08, slave on IR2 base 0x70
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
    // a 3-byte AUX packet raises IRQ12 per byte and the controller re-asserts inside the same in(0x60);
    // the PIC in-service block keeps INT 74h from re-entering
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
        // one interrupt per byte, none while one is in service
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
    // exactly three bytes
    for (int i = 0; i < 64; ++i) cs.tick(cycles += 8, 66000000.0);
    EXPECT_EQ(cs.poll_interrupt(), -1);
}

TEST(ChipsetTest, SoundBlasterPlaybackDmaReadsMemoryIntoTheCard) {
    // a D/A transfer moves memory into the card, not the other way
    Chipset cs;
    const uint32_t phys = 0x00012000;
    const uint8_t pcm[8] = {0x80, 0xFF, 0x00, 0x80, 0xC0, 0x40, 0x80, 0x80};
    for (uint32_t i = 0; i < 8; ++i) cs.mem_write(phys + i, pcm[i]);

    // DMA1 channel 1, mode 0x49 (single, increment, read from memory)
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
    // playback must not write memory
    for (uint32_t i = 0; i < 8; ++i) EXPECT_EQ(cs.mem_read(phys + i), pcm[i]) << "byte " << i;
}

TEST(ChipsetTest, SoundBlasterRecordingDmaWritesTheCardsSamplesIntoMemory) {
    // A/D transfer: unplugged inputs record unsigned-8-bit silence
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

TEST(ChipsetTest, SoundBlasterPlaybackWrapsWithinTheSixtyFourKPageNotAcrossIt) {
    // the 8-bit address register wraps within its 64KB page (8237, dma8237.h Channel), not into the next
    Chipset cs;
    const uint32_t page = 0x00020000;      // page-aligned
    const uint32_t start = page + 0xFFFE;  // last 2 bytes of the page
    const uint8_t pcm[4] = {0xA1, 0xB2, 0xC3, 0xD4};
    cs.mem_write(start, pcm[0]);
    cs.mem_write(start + 1, pcm[1]);
    cs.mem_write(page, pcm[2]);
    cs.mem_write(page + 1, pcm[3]);

    cs.io_out(0x0C, 0x00);
    cs.io_out(0x02, uint8_t(start & 0xFF));
    cs.io_out(0x02, uint8_t((start >> 8) & 0xFF));
    cs.io_out(0x0C, 0x00);
    cs.io_out(0x03, 0x03);  // count = 4 - 1
    cs.io_out(0x03, 0x00);
    cs.io_out(0x83, uint8_t(page >> 16));
    cs.io_out(0x0B, 0x49);  // single transfer, read-from-memory, channel 1
    cs.io_out(0x0A, 0x01);

    cs.io_out(0x226, 1);
    cs.io_out(0x226, 0);
    ASSERT_EQ(cs.io_in(0x22A), 0xAA);
    cs.io_out(0x22C, 0x40); cs.io_out(0x22C, 166);
    cs.io_out(0x22C, 0x14); cs.io_out(0x22C, 0x03); cs.io_out(0x22C, 0x00);  // 4 bytes

    for (uint64_t c = 0; c < 2'000'000 && cs.sb.playing(); c += 64) cs.tick(c, 66000000.0);
    EXPECT_FALSE(cs.sb.playing());

    auto samples = cs.sb.drain_samples();
    ASSERT_EQ(samples.size(), 4u);
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(samples[i].left, int16_t((int(pcm[i]) - 128) * 256)) << "byte " << i;
}

TEST(ChipsetTest, DmaIdentificationWritesOneByteToMemoryWithNoInterrupt) {
    // E2h's single-byte DMA write is not a DSP block: no interrupt, no counters
    Chipset cs;
    InitPics(cs);
    const uint32_t phys = 0x00015000;
    cs.mem_write(phys, 0x00);

    cs.io_out(0x0C, 0x00);
    cs.io_out(0x02, uint8_t(phys & 0xFF));
    cs.io_out(0x02, uint8_t((phys >> 8) & 0xFF));
    cs.io_out(0x0C, 0x00);
    cs.io_out(0x03, 0x00);  // count = 1 - 1
    cs.io_out(0x03, 0x00);
    cs.io_out(0x83, uint8_t(phys >> 16));
    cs.io_out(0x0B, 0x45);  // single transfer, write-to-memory, channel 1
    cs.io_out(0x0A, 0x01);

    cs.io_out(0x226, 1);
    cs.io_out(0x226, 0);
    ASSERT_EQ(cs.io_in(0x22A), 0xAA);
    cs.io_out(0x22C, 0xE2); cs.io_out(0x22C, 0x12);  // DMA identification, p = 0x12

    for (uint64_t c = 0; c < 2000 && cs.sb.transfer_ready(); c += 64) cs.tick(c, 66000000.0);
    EXPECT_FALSE(cs.sb.transfer_ready());
    EXPECT_EQ(cs.mem_read(phys), uint8_t(0xAA + (0x12 ^ 0x96)));  // valadd=0xAA, valxor=0x96 out of reset
    EXPECT_EQ(cs.poll_interrupt(), -1);
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

TEST(ChipsetTest, SoundBlasterHonoursTheMixerSelectedIrqLine) {
    // mixer 80h selects which PIC line the card drives
    Chipset cs;
    uint64_t cycles = 0;

    auto select_and_trigger = [&](uint8_t mixer80) {
        InitPics(cs);  // fresh master (vector base 8) and slave (0x70), fully unmasked
        cs.io_out(0x226, 1);
        cs.io_out(0x226, 0);
        ASSERT_EQ(cs.io_in(0x22A), 0xAA);
        // flush sb_irq_prev_ edge state first; it only catches up on the next tick()
        cs.tick(cycles += 64, 66000000.0);
        cs.io_out(0x224, 0x80);
        cs.io_out(0x225, mixer80);
        cs.io_out(0x22C, 0xF2);  // F2h: diagnostic 8-bit-source interrupt trigger
    };
    auto run_to_vector = [&]() {
        int vec = -1;
        for (int i = 0; i < 2000 && vec < 0; ++i) { cs.tick(cycles += 64, 66000000.0); vec = cs.poll_interrupt(); }
        return vec;
    };

    select_and_trigger(0x02);  // bit1 = IRQ5 (the default), master PIC
    EXPECT_EQ(run_to_vector(), 0x08 + 5);

    select_and_trigger(0x04);  // bit2 = IRQ7, master PIC
    EXPECT_EQ(run_to_vector(), 0x08 + 7);

    select_and_trigger(0x08);  // bit3 = IRQ10 = global IRQ10 = slave line 2
    EXPECT_EQ(run_to_vector(), 0x70 + 2);

    // bit0 is the "IRQ2" jumper; on an AT it routes to slave IR1 (IRQ9) (IBM 5170 Technical Reference)
    select_and_trigger(0x01);
    EXPECT_EQ(run_to_vector(), 0x70 + 1);

    // no bit set: no interrupt
    select_and_trigger(0x00);
    for (int i = 0; i < 100; ++i) cs.tick(cycles += 64, 66000000.0);
    EXPECT_EQ(cs.poll_interrupt(), -1);
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

TEST(ChipsetTest, Mpu401AnswersItsOwnPortsThroughTheBusDecode) {
    // MPU-401 sits at its own base, SBPG Appendix A Table A-16 default 330h/331h
    Chipset cs;
    EXPECT_FALSE(cs.sb.owns(0x330)) << "the card's own block must not cover the MPU-401";
    EXPECT_FALSE(cs.mpu.owns(0x220));
    // detection probe: FFh to command, poll status, read FEh
    cs.io_out(0x331, 0xFF);
    EXPECT_EQ(cs.io_in(0x331) & 0x80, 0x00) << "status must report input data available";
    EXPECT_EQ(cs.io_in(0x330), 0xFE);
    cs.io_out(0x331, 0x3F);
    EXPECT_EQ(cs.io_in(0x330), 0xFE);
    EXPECT_TRUE(cs.mpu.uart_mode());
}
