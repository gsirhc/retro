// GoogleTest suite for the AT glue-logic layer: memory read/write and ROM
// write-protection, the A20 gate's effect on wraparound vs. open-bus
// behavior, port I/O dispatch to the owned devices, and the master/slave
// PIC cascade through poll_interrupt().

#include <gtest/gtest.h>

#include "chipset.h"

#include <vector>

namespace {

using ibmpcat::Chipset;

TEST(ChipsetTest, MemoryReadWriteRoundTrip) {
    Chipset cs;
    cs.mem[0x1234] = 0;
    auto bus = cs.make_bus();
    bus.write(0x1234, 0xAB);
    EXPECT_EQ(bus.read(0x1234), 0xAB);
}

TEST(ChipsetTest, RomRegionRejectsWrites) {
    Chipset cs;
    uint8_t rom[4] = {0x11, 0x22, 0x33, 0x44};
    cs.load_rom(0xF0000, rom, 4);
    auto bus = cs.make_bus();
    EXPECT_EQ(bus.read(0xF0000), 0x11);
    bus.write(0xF0000, 0xFF);
    EXPECT_EQ(bus.read(0xF0000), 0x11);  // unchanged -- ROM ignores writes
}

TEST(ChipsetTest, A20DisabledWrapsAt1MB) {
    Chipset cs;
    auto bus = cs.make_bus();
    // A20 starts disabled (see i8042.h): address 0x100010 should alias 0x10.
    bus.write(0x000010, 0x77);
    EXPECT_EQ(bus.read(0x100010), 0x77);
}

TEST(ChipsetTest, A20EnabledDoesNotWrapAndAboveOneMebIsOpenBus) {
    Chipset cs;
    auto bus = cs.make_bus();
    bus.write(0x000010, 0x77);
    // Enable A20 via the keyboard controller's write-output-port command.
    cs.kbc.out(0x64, 0xD1);
    cs.kbc.out(0x60, 0x02);  // bit1 set -> A20 enabled
    ASSERT_TRUE(cs.kbc.a20_enabled());
    EXPECT_EQ(bus.read(0x100010), 0xFF);  // no wraparound; nothing populated up there -> open bus
    EXPECT_EQ(bus.read(0x000010), 0x77);  // low memory unaffected
}

TEST(ChipsetTest, Port61GatesSpeakerAndReadsBackRefreshToggle) {
    Chipset cs;
    auto bus = cs.make_bus();
    bus.out(0x61, 0x01);  // gate channel 2 on
    uint8_t before = bus.in(0x61);
    cs.tick(1000, 1193182.0);
    uint8_t after = bus.in(0x61);
    EXPECT_NE(before & 0x10, after & 0x10);  // refresh toggle flipped by tick()
    EXPECT_EQ(before & 0x01, 0x01);          // gate bit reads back as programmed
}

TEST(ChipsetTest, DmaPageRegisterRoundTripsThroughPorts) {
    Chipset cs;
    auto bus = cs.make_bus();
    bus.out(0x81, 0x05);  // DMA1 channel 2 (floppy) page register
    EXPECT_EQ(bus.in(0x81), 0x05);
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
    Chipset cs;
    std::vector<uint8_t> img(80 * 2 * 15 * 512, 0);
    img[512] = 0x77;  // sector 2 (1-based), first byte
    cs.fdc.mount(0, img.data(), img.size());
    cs.fdc.out(0x3F2, 0x14);  // DOR: ~RESET high, motor A on, drive 0 select
    // DMA1 channel 2: address 0x2000, page 0, count 511 (512 bytes, N-1),
    // read mode (device -> memory), unmasked.
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
        cs.tick(c, 8000000.0);
        if (cs.mem[0x2000] == 0x77) copied = true;
    }
    EXPECT_TRUE(copied);
    EXPECT_EQ(cs.dma1.address(2), 0x2000 + 512);
    EXPECT_TRUE(cs.fdc.irq_pending());
}

TEST(ChipsetTest, Irq6IsEdgeTriggeredNotReRaisedWhileStillPending) {
    // Real ISA IRQ6 is edge-triggered: it fires once, on the transition,
    // not continuously for as long as the underlying condition holds.
    // Modeling it as level-triggered (re-raising every tick while
    // fdc.irq_pending() stays true) turned a real BIOS interrupt handler
    // that legitimately returns without draining FDC result bytes itself
    // into an infinite interrupt storm -- an actual bug this session hit
    // trying to boot real BIOS + FreeDOS. See IBM_PCAT_REVIEW.md §8.
    Chipset cs;
    cs.pic_master.out(0x20, 0x11);
    cs.pic_master.out(0x21, 0x08);
    cs.pic_master.out(0x21, 0x04);
    cs.pic_master.out(0x21, 0x01);
    cs.pic_master.out(0x21, 0x00);  // unmask all, vector base 8

    std::vector<uint8_t> img(80 * 2 * 15 * 512, 0);
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
        cs.tick(c, 8000000.0);
        if (cs.fdc.irq_pending()) break;
    }
    ASSERT_TRUE(cs.fdc.irq_pending());

    // Service the interrupt exactly once, as the CPU would, WITHOUT
    // draining any FDC result bytes -- modeling the real BIOS ISR path
    // that just EOIs and returns. fdc.irq_pending() stays true throughout
    // (nothing drained it), but IRQ6 must NOT still be pending at the PIC
    // afterward, since real hardware only latched the one edge.
    int vec1 = cs.poll_interrupt();
    ASSERT_GE(vec1, 0);
    cs.pic_master.out(0x20, 0x20);  // non-specific EOI, as the ISR would issue
    EXPECT_TRUE(cs.fdc.irq_pending());  // still not drained
    EXPECT_FALSE(cs.pic_master.has_interrupt());  // but no new edge -> nothing re-pending

    cs.tick(20'000'000, 8000000.0);  // even after more ticks with the condition still true
    EXPECT_FALSE(cs.pic_master.has_interrupt());
}

TEST(ChipsetTest, KeyboardIrq1ReachesThePic) {
    // IRQ1 (keyboard) was never wired to the PIC at all until an actual
    // end-to-end FreeDOS boot test caught it: a keypress sat in i8042's
    // output buffer forever because nothing ever told the PIC about it,
    // so software waiting on the vectored keyboard interrupt (rather than
    // polling port 0x60 directly) never woke up. See IBM_PCAT_REVIEW.md §9.
    Chipset cs;
    cs.pic_master.out(0x20, 0x11);
    cs.pic_master.out(0x21, 0x08);
    cs.pic_master.out(0x21, 0x04);
    cs.pic_master.out(0x21, 0x01);
    cs.pic_master.out(0x21, 0x00);  // unmask all, vector base 8
    cs.kbc.out(0x64, 0x60); cs.kbc.out(0x60, 0x01);  // command byte: enable IRQ1

    cs.kbc.inject_scancode(0x1E);  // 'A' make code
    ASSERT_TRUE(cs.kbc.irq1_pending());
    cs.tick(0, 8000000.0);
    EXPECT_TRUE(cs.pic_master.has_interrupt());
    EXPECT_EQ(cs.poll_interrupt(), 0x08 + 1);  // vector_base(8) + IR1
}

TEST(ChipsetTest, HddIdentifyAndReadGoesThroughAtomicSixteenBitBusPath) {
    // End-to-end through the chipset's own bus (not Wd1003 directly): proves
    // the CPU-facing IN AX,DX / OUT DX,AX path (bus.in16/out16) reaches the
    // real hard disk data register at 0x1F0 rather than being decomposed
    // into two 8-bit accesses, which would read the unrelated Error
    // register as the "high byte". See cpu80286.h/chipset.h's in16/out16
    // comments and IBM_PCAT_REVIEW.md.
    Chipset cs;
    auto bus = cs.make_bus();
    std::vector<uint8_t> img(733 * 5 * 17 * 512, 0);
    img[512] = 0x42;  // sector 2 (1-based), first byte
    cs.hdd.mount(0, img.data(), img.size());

    bus.out(0x1F6, 0xA0);  // drive 0, head 0
    bus.out(0x1F7, 0xEC);  // IDENTIFY DEVICE
    ASSERT_TRUE(bus.in(0x1F7) & 0x08);  // DRQ
    EXPECT_EQ(bus.in16(0x1F0), 0x0040);  // word 0: general config, fixed device
    EXPECT_EQ(bus.in16(0x1F0), 733);     // word 1: cylinders
    for (int i = 0; i < 254; ++i) bus.in16(0x1F0);  // drain the rest

    bus.out(0x1F6, 0xA0);
    bus.out(0x1F2, 1);  // 1 sector
    bus.out(0x1F3, 2);  // sector 2 (1-based)
    bus.out(0x1F4, 0); bus.out(0x1F5, 0);
    bus.out(0x1F7, 0x20);  // READ SECTORS

    bool ready = false;
    for (uint64_t c = 0; c < 10'000'000 && !ready; c += 100) {
        cs.tick(c, 8000000.0);
        if (bus.in(0x3F6) & 0x08) ready = true;
    }
    ASSERT_TRUE(ready);
    EXPECT_EQ(bus.in16(0x1F0) & 0xFF, 0x42);
}

TEST(ChipsetTest, Irq14IsEdgeTriggeredNotReRaisedWhilePending) {
    // Same edge-triggering discipline as IRQ6/IRQ1 (see those tests above),
    // for IRQ14 (hard disk), which lives on the slave PIC's line 6.
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

    std::vector<uint8_t> img(733 * 5 * 17 * 512, 0);
    cs.hdd.mount(0, img.data(), img.size());
    cs.hdd.out(0x1F6, 0xA0);
    cs.hdd.out(0x1F2, 1); cs.hdd.out(0x1F3, 1);
    cs.hdd.out(0x1F4, 0); cs.hdd.out(0x1F5, 0);
    cs.hdd.out(0x1F7, 0x20);  // READ SECTORS

    for (uint64_t c = 0; c < 10'000'000; c += 100) {
        cs.tick(c, 8000000.0);
        if (cs.hdd.irq_pending()) break;
    }
    ASSERT_TRUE(cs.hdd.irq_pending());

    // Service exactly once WITHOUT reading the HDC's status register (which
    // would itself acknowledge the interrupt) -- e.g. an ISR that only EOIs.
    int vec1 = cs.poll_interrupt();
    ASSERT_GE(vec1, 0);
    cs.pic_slave.out(0xA0, 0x20);   // non-specific EOI on the slave
    cs.pic_master.out(0x20, 0x20);  // ...and the cascaded master EOI
    EXPECT_TRUE(cs.hdd.irq_pending());              // still not acknowledged at the device
    EXPECT_FALSE(cs.pic_slave.has_interrupt());     // but no new edge -> nothing re-pending

    cs.tick(20'000'000, 8000000.0);  // even after more ticks with the condition still true
    EXPECT_FALSE(cs.pic_slave.has_interrupt());
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
