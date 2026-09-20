// GoogleTest suite for the secondary-channel ATAPI CD-ROM drive: the
// post-reset ATAPI signature a BIOS/driver detects a packet device by,
// IDENTIFY PACKET DEVICE, the PACKET command protocol (byte-count-limited
// PIO data blocks, the Interrupt Reason register's phase encoding), every
// implemented SCSI-3 MMC CDB, and the eject/re-insert media-change path a
// DOS CD-ROM driver polls.
//
// Register accesses go through in()/out()/data_in16()/data_out16() exactly
// as the chipset's port decode will, so these tests exercise the real
// programming model rather than internal helpers -- the same convention
// wd1003_test.cpp uses.

#include <gtest/gtest.h>

#include "atapi_cdrom.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using pc486::AtapiCdrom;

constexpr int kSectorBytes = AtapiCdrom::kBytesPerSector;

// An ISO image of `blocks` 2048-byte sectors, each stamped with its own LBA
// in the first two bytes so a test can confirm exactly which sector came
// back.
std::vector<uint8_t> MakeIso(uint32_t blocks) {
    std::vector<uint8_t> img(std::size_t(blocks) * kSectorBytes, 0);
    for (uint32_t b = 0; b < blocks; ++b) {
        img[std::size_t(b) * kSectorBytes] = uint8_t(b & 0xFF);
        img[std::size_t(b) * kSectorBytes + 1] = uint8_t((b >> 8) & 0xFF);
    }
    return img;
}

class AtapiCdromTest : public ::testing::Test {
protected:
    AtapiCdrom cd;
    uint64_t cycles_ = 0;

    void SetUp() override { cd.reset(); }

    struct Sense {
        uint8_t key = 0, asc = 0, ascq = 0;
    };

    void SelectDevice0() { cd.out(0x176, 0xA0); }

    uint8_t AltStatus() { return cd.in(0x376); }  // does NOT acknowledge the interrupt
    bool Busy() { return (AltStatus() & 0x80) != 0; }
    bool Drq() { return (AltStatus() & 0x08) != 0; }
    bool CheckCondition() { return (AltStatus() & 0x01) != 0; }
    uint16_t ByteCount() { return uint16_t(cd.in(0x174) | (uint16_t(cd.in(0x175)) << 8)); }
    uint8_t IntReason() { return uint8_t(cd.in(0x172) & 0x07); }

    // Poll BSY the way an ATAPI driver does (DRDY is never a valid "ready"
    // signal on a packet device -- see atapi_cdrom.h).
    void RunToIdle() {
        for (int i = 0; i < 100000 && Busy(); ++i) {
            cycles_ += 100000;
            cd.tick(cycles_);
        }
        ASSERT_FALSE(Busy()) << "command never completed";
    }

    // Issue one PACKET command: byte-count limit, 0xA0, then the 12-byte CDB
    // through the 16-bit data register.
    void SendPacket(std::vector<uint8_t> cdb, uint16_t limit = 0xFFFE) {
        cdb.resize(12, 0);
        cd.out(0x174, uint8_t(limit & 0xFF));
        cd.out(0x175, uint8_t(limit >> 8));
        cd.out(0x171, 0x00);  // Features: PIO, no overlap
        cd.out(0x177, 0xA0);  // PACKET
        ASSERT_TRUE(Drq()) << "device must request the command packet via DRQ";
        ASSERT_EQ(IntReason(), 0x01) << "C/D=1, I/O=0 while the packet is requested";
        for (int i = 0; i < 12; i += 2) {
            cd.data_out16(uint16_t(uint16_t(cdb[std::size_t(i)]) |
                                   (uint16_t(cdb[std::size_t(i) + 1]) << 8)));
        }
        RunToIdle();
    }

    // Drain every data block the device offers, honoring the byte count it
    // publishes for each one.
    std::vector<uint8_t> DrainData() {
        std::vector<uint8_t> out;
        int blocks = 0;
        while (Drq()) {
            EXPECT_EQ(IntReason(), 0x02) << "I/O=1, C/D=0 for a data-in block";
            uint16_t count = ByteCount();
            EXPECT_EQ(count % 2, 0) << "ATAPI byte counts are always even";
            for (uint16_t i = 0; i < count; i += 2) {
                uint16_t w = cd.data_in16();
                out.push_back(uint8_t(w & 0xFF));
                out.push_back(uint8_t(w >> 8));
            }
            if (++blocks > 4096) break;
        }
        return out;
    }

    int CountDataBlocks() {
        int blocks = 0;
        while (Drq()) {
            ++blocks;
            uint16_t count = ByteCount();
            for (uint16_t i = 0; i < count; i += 2) cd.data_in16();
            if (blocks > 4096) break;
        }
        return blocks;
    }

    Sense RequestSense() {
        SendPacket({0x03, 0, 0, 0, 18});
        auto d = DrainData();
        Sense s;
        if (d.size() >= 14) {
            s.key = uint8_t(d[2] & 0x0F);
            s.asc = d[12];
            s.ascq = d[13];
        }
        return s;
    }

    // Every real drive reports a unit attention for the reset itself on the
    // first command after power-on (SPC 5.6). Consume it so a test can look
    // at the condition it actually cares about.
    void ConsumeResetUnitAttention() {
        SendPacket({0x00});  // TEST UNIT READY
        RequestSense();
    }

    void MountDisc(uint32_t blocks) {
        auto img = MakeIso(blocks);
        cd.mount(img.data(), img.size());
    }
};

TEST_F(AtapiCdromTest, OwnsSecondaryChannelPortsOnly) {
    for (uint16_t p = 0x170; p <= 0x177; ++p) EXPECT_TRUE(cd.owns(p)) << std::hex << p;
    EXPECT_TRUE(cd.owns(0x376));
    // The primary channel (the hard disk, see wd1003.h) and the floppy
    // controller's 0x377 must stay out of this device's decode -- the whole
    // point of putting the CD-ROM on the secondary channel is that the two
    // controllers are independent.
    EXPECT_FALSE(cd.owns(0x1F0));
    EXPECT_FALSE(cd.owns(0x3F6));
    EXPECT_FALSE(cd.owns(0x377));
    EXPECT_FALSE(cd.owns(0x16F));
    EXPECT_FALSE(cd.owns(0x178));
}

TEST_F(AtapiCdromTest, AtapiSignatureAfterReset) {
    // ATA/ATAPI-4 section 9.1 "Signature and persistence": a packet device
    // reports Sector Count/Number = 01h and Cylinder Low/High = 14h/EBh.
    // This is the single mechanism a BIOS uses to tell a CD-ROM from a hard
    // disk on an otherwise identical register block, so it is the most
    // load-bearing fact in the whole device.
    EXPECT_EQ(cd.in(0x172), 0x01);
    EXPECT_EQ(cd.in(0x173), 0x01);
    EXPECT_EQ(cd.in(0x174), 0x14);
    EXPECT_EQ(cd.in(0x175), 0xEB);
    // ...and Status reads 00h: DRDY is deliberately NOT set. An ATAPI device
    // genuinely never reports itself "ready" the way a disk does, which is
    // why an ATAPI driver polls BSY instead. A host that also checks
    // Status != 0 (as the legacy BIOS's ATA branch does) therefore cannot
    // mistake this for an ATA disk even before looking at the cylinder
    // registers.
    EXPECT_EQ(cd.in(0x376), 0x00);
    EXPECT_EQ(cd.in(0x171), 0x01);  // post-reset "diagnostics passed" error code
}

TEST_F(AtapiCdromTest, ScratchRegisterProbeReadsBackThroughTheSharedLatches) {
    // The legacy-BIOS detection path writes 0x55/0xAA to Sector
    // Count/Sector Number and requires them to read back before it will even
    // attempt the reset/signature probe. On a real ATAPI device the register
    // at offset 2 is one physical latch -- written as Sector Count, read as
    // the Interrupt Reason register -- which the device only overwrites at a
    // phase transition, so the read-back works. Modeling offset 2 as a
    // read-only phase register instead would make this device invisible to
    // that detection code.
    SelectDevice0();
    cd.out(0x172, 0x55);
    cd.out(0x173, 0xAA);
    EXPECT_EQ(cd.in(0x172), 0x55);
    EXPECT_EQ(cd.in(0x173), 0xAA);
    cd.out(0x172, 0xAA);
    cd.out(0x173, 0x55);
    EXPECT_EQ(cd.in(0x172), 0xAA);
    EXPECT_EQ(cd.in(0x173), 0x55);
}

TEST_F(AtapiCdromTest, SoftResetReassertsAtapiSignature) {
    cd.out(0x172, 0x55);
    cd.out(0x173, 0xAA);
    cd.out(0x174, 0x11);
    cd.out(0x175, 0x22);
    cd.out(0x376, 0x04);  // assert SRST
    cd.out(0x376, 0x00);  // release it -- real hardware resets on this edge
    EXPECT_EQ(cd.in(0x172), 0x01);
    EXPECT_EQ(cd.in(0x173), 0x01);
    EXPECT_EQ(cd.in(0x174), 0x14);
    EXPECT_EQ(cd.in(0x175), 0xEB);
    EXPECT_EQ(cd.in(0x376), 0x00);
}

TEST_F(AtapiCdromTest, DeviceZeroRespondsForTheAbsentDeviceOneWithZeroes) {
    // ATA/ATAPI-6 (T13/1410D revision 3a) Table 18, "Device 1 is selected
    // and Device 0 is responding for Device 1": a device implementing the
    // PACKET command set places 00h on the bus for Sector Count, LBA
    // Low/Mid/High and the Device register, and 00h for Status and
    // Alternate Status. A non-packet device places its own register
    // contents instead, which is why wd1003 legitimately behaves
    // differently -- both are correct for their own device type.
    //
    // Load-bearing, not pedantry: FreeDOS's real ATAPICDD.SYS probes device
    // 1 by writing 0x55/0xAA to Sector Count/Number and reading them back,
    // then by checking Sector Count/Number == 01h/01h after a reset before
    // testing for the 14h/EBh signature. Answering either probe with this
    // device's own registers invents a phantom ATAPI slave for the driver
    // to time out against.
    cd.out(0x176, 0xB0);  // select device 1 -- permanently unpopulated
    cd.out(0x172, 0x55);
    cd.out(0x173, 0xAA);
    EXPECT_EQ(cd.in(0x172), 0x00) << "the scratch probe must not read back";
    EXPECT_EQ(cd.in(0x173), 0x00);

    cd.out(0x376, 0x04);
    cd.out(0x376, 0x00);  // a reset issued here still resets device 0...
    EXPECT_EQ(cd.in(0x172), 0x00) << "...but device 1's task file still reads 00h";
    EXPECT_EQ(cd.in(0x173), 0x00);
    EXPECT_EQ(cd.in(0x174), 0x00) << "no ATAPI signature for a position nothing occupies";
    EXPECT_EQ(cd.in(0x175), 0x00);
    EXPECT_EQ(cd.in(0x176), 0x00);
    EXPECT_EQ(cd.in(0x177), 0x00) << "Status reads 00h";
    EXPECT_EQ(cd.in(0x376), 0x00) << "Alternate Status too";
    // The Error register is the one exception in Table 18: it reports
    // device 0's own contents.
    EXPECT_EQ(cd.in(0x171), 0x01);

    // Selecting the real device 0 brings its genuine signature straight
    // back -- the 00h answers are a property of who is selected, not a
    // window that expires or a state that sticks.
    SelectDevice0();
    EXPECT_EQ(cd.in(0x172), 0x01);
    EXPECT_EQ(cd.in(0x173), 0x01);
    EXPECT_EQ(cd.in(0x174), 0x14);
    EXPECT_EQ(cd.in(0x175), 0xEB);
}

TEST_F(AtapiCdromTest, ExecuteDeviceDiagnosticIsTheOneCommandTheAbsentDeviceOneAnswers) {
    // Table 18's command-register row: "Place new data into the Command
    // register of Device 0. Do not respond unless the command is EXECUTE
    // DEVICE DIAGNOSTICS." That exception is how a host gets a diagnostic
    // result covering both device positions from a single-device channel.
    cd.out(0x176, 0xB0);
    cd.out(0x177, 0xA1);  // IDENTIFY PACKET DEVICE: ignored
    EXPECT_FALSE(cd.irq_pending());

    cd.out(0x177, 0x90);  // EXECUTE DEVICE DIAGNOSTIC: answered
    EXPECT_TRUE(cd.irq_pending());
    SelectDevice0();
    EXPECT_EQ(cd.in(0x171), 0x01) << "diagnostics passed";
    EXPECT_EQ(cd.in(0x174), 0x14) << "and the signature is reasserted";
    EXPECT_EQ(cd.in(0x175), 0xEB);
}

TEST_F(AtapiCdromTest, CommandWriteToTheAbsentDeviceOneDoesNothing) {
    // Only the command register write is gated on device selection -- the
    // one access that decides which device's command logic responds. There
    // is no device 1 state machine here, so the command simply never
    // executes: no DRQ, no interrupt, no status change.
    cd.out(0x176, 0xB0);
    cd.out(0x177, 0xA1);  // IDENTIFY PACKET DEVICE aimed at device 1
    EXPECT_FALSE(Drq());
    EXPECT_FALSE(cd.irq_pending());

    SelectDevice0();
    cd.out(0x177, 0xA1);
    EXPECT_TRUE(Drq());
    EXPECT_TRUE(cd.irq_pending());
}

TEST_F(AtapiCdromTest, IdentifyPacketDeviceDescribesAPacketCdRomDevice) {
    SelectDevice0();
    cd.out(0x177, 0xA1);
    ASSERT_TRUE(Drq());
    std::vector<uint16_t> w;
    for (int i = 0; i < 256; ++i) w.push_back(cd.data_in16());
    EXPECT_FALSE(Drq()) << "DRQ clears once the 512-byte block is drained";

    // Word 0's general configuration is where this genuinely differs from an
    // ATA disk's IDENTIFY DEVICE (wd1003 reports 0x0040, "fixed device") --
    // ATA/ATAPI-4 section 8.13.8, Table 12.
    EXPECT_EQ(w[0] & 0xC000, 0x8000) << "bits 15:14 = 10b: ATAPI device";
    EXPECT_EQ((w[0] >> 8) & 0x1F, 0x05) << "CD-ROM command set (SCSI-3 MMC)";
    EXPECT_TRUE(w[0] & 0x0080) << "removable media";
    EXPECT_EQ(w[0] & 0x0003, 0x0000) << "12-byte command packet";
    EXPECT_EQ((w[0] >> 5) & 0x03, 0x02) << "accelerated DRQ -- must agree with "
                                           "begin_packet() not raising an IRQ "
                                           "for the packet-request phase";

    // Word 49: LBA supported, and DMA deliberately NOT advertised -- this
    // device is PIO-only, and a driver told otherwise would program a
    // bus-master controller this machine does not have.
    EXPECT_TRUE(w[49] & 0x0200) << "LBA supported";
    EXPECT_FALSE(w[49] & 0x0100) << "DMA must not be advertised";
    EXPECT_EQ(w[63], 0x0000) << "no multiword DMA modes";
    EXPECT_TRUE(w[82] & 0x0010) << "PACKET command feature set supported";
    EXPECT_TRUE(w[82] & 0x0200) << "DEVICE RESET supported";
    EXPECT_TRUE(w[83] & 0x4000) << "words 82-84 marked valid";

    // Model number, words 27-46: ATA string fields byte-swap each character
    // pair within its word (ATA/ATAPI-4 section 8.12.8).
    std::string model;
    for (int i = 27; i <= 46; ++i) {
        model.push_back(char(w[std::size_t(i)] >> 8));
        model.push_back(char(w[std::size_t(i)] & 0xFF));
    }
    EXPECT_EQ(model.substr(0, 18), "RETROWEB CD-ROM 2X");
}

TEST_F(AtapiCdromTest, IdentifyDeviceIsAbortedWithTheAtapiSignature) {
    // ATA/ATAPI-4 section 8.12.1: a packet device aborts IDENTIFY DEVICE
    // (0xEC) and places its signature in the task file. That is the second
    // half of device-type detection -- a host that issues the ATA identify
    // without checking the reset signature first still learns what it is
    // talking to instead of hanging or reading a bogus geometry.
    SelectDevice0();
    cd.out(0x177, 0xEC);
    EXPECT_FALSE(Drq()) << "no identify data for the ATA form of the command";
    EXPECT_TRUE(CheckCondition());
    EXPECT_TRUE(cd.irq_pending());
    uint8_t err = cd.in(0x171);
    EXPECT_TRUE(err & 0x04) << "ABRT";
    EXPECT_EQ(err >> 4, 0x05) << "sense key in the Error register's high nibble: ILLEGAL REQUEST";
    EXPECT_EQ(cd.in(0x174), 0x14);
    EXPECT_EQ(cd.in(0x175), 0xEB);
}

TEST_F(AtapiCdromTest, DeviceResetRestoresTheSignatureAndRaisesNoInterrupt) {
    // ATA/ATAPI-4 section 8.7: DEVICE RESET is a packet-device-only command
    // that resets protocol state and reasserts the signature, and
    // deliberately does NOT assert INTRQ -- a driver polls BSY for it.
    SelectDevice0();
    cd.out(0x177, 0xA1);  // leave a data-in phase in progress
    ASSERT_TRUE(Drq());
    cd.in(0x177);         // acknowledge the identify's interrupt

    cd.out(0x177, 0x08);  // DEVICE RESET
    EXPECT_FALSE(Drq()) << "the in-progress data phase is abandoned";
    EXPECT_FALSE(cd.irq_pending());
    EXPECT_EQ(cd.in(0x376), 0x00);
    EXPECT_EQ(cd.in(0x174), 0x14);
    EXPECT_EQ(cd.in(0x175), 0xEB);
}

TEST_F(AtapiCdromTest, UnsupportedAtaCommandIsAborted) {
    SelectDevice0();
    cd.out(0x177, 0x20);  // READ SECTORS -- an ATA disk command, meaningless here
    EXPECT_TRUE(CheckCondition());
    EXPECT_TRUE(cd.in(0x171) & 0x04) << "ABRT";
}

TEST_F(AtapiCdromTest, PowerOnResetRaisesAUnitAttention) {
    // SPC section 5.6: a reset creates a unit-attention condition, reported
    // as CHECK CONDITION on the first command after it and then cleared.
    // Real drives do exactly this, which is why DOS CD-ROM drivers issue
    // TEST UNIT READY twice at startup.
    MountDisc(64);
    cd.reset();
    SelectDevice0();
    SendPacket({0x00});  // TEST UNIT READY
    EXPECT_TRUE(CheckCondition());
    Sense s = RequestSense();
    EXPECT_EQ(s.key, 0x06) << "UNIT ATTENTION";
    EXPECT_EQ(s.asc, 0x29) << "POWER ON, RESET, OR BUS DEVICE RESET OCCURRED";
    EXPECT_EQ(s.ascq, 0x00);

    // Cleared once reported: the next TEST UNIT READY succeeds outright.
    SendPacket({0x00});
    EXPECT_FALSE(CheckCondition());
    EXPECT_EQ(IntReason(), 0x03) << "I/O=1, C/D=1: command complete";
}

TEST_F(AtapiCdromTest, TestUnitReadyReportsMediumNotPresentWithAnEmptyTray) {
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x00});
    EXPECT_TRUE(CheckCondition());
    Sense s = RequestSense();
    EXPECT_EQ(s.key, 0x02) << "NOT READY";
    EXPECT_EQ(s.asc, 0x3A) << "MEDIUM NOT PRESENT";
    EXPECT_EQ(s.ascq, 0x00);
}

TEST_F(AtapiCdromTest, RequestSenseItselfNeverFailsAndClearsTheSenseData) {
    // REQUEST SENSE is how a driver finds out why something failed, so it
    // must succeed even with no disc in the tray -- and per SPC section 7.20
    // reading the sense data clears it, so a second read reports NO SENSE.
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x00});  // fails: no media
    Sense first = RequestSense();
    EXPECT_EQ(first.key, 0x02);
    EXPECT_FALSE(CheckCondition()) << "REQUEST SENSE itself succeeded";
    Sense second = RequestSense();
    EXPECT_EQ(second.key, 0x00) << "NO SENSE";
    EXPECT_EQ(second.asc, 0x00);
}

TEST_F(AtapiCdromTest, InquiryIdentifiesACdRomWithNoDiscLoaded) {
    // INQUIRY describes the drive, not the medium, so it succeeds with an
    // empty tray -- and it is exempt from the unit-attention report (SPC
    // section 5.6), which is why this test does not consume one first.
    SelectDevice0();
    SendPacket({0x12, 0, 0, 0, 36});
    EXPECT_FALSE(CheckCondition());
    auto d = DrainData();
    ASSERT_GE(d.size(), 36u);
    EXPECT_EQ(d[0] & 0x1F, 0x05) << "peripheral device type: CD-ROM";
    EXPECT_TRUE(d[1] & 0x80) << "RMB: removable medium";
    EXPECT_EQ(d[4], 31) << "additional length -> 36 bytes total";
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(&d[8]), 8), "RETROWEB");
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(&d[16]), 9), "CD-ROM 2X");
}

TEST_F(AtapiCdromTest, InquiryHonorsAShortAllocationLength) {
    SelectDevice0();
    SendPacket({0x12, 0, 0, 0, 5});
    auto d = DrainData();
    EXPECT_EQ(d.size(), 6u) << "5 bytes requested, padded up to a whole 16-bit word";
}

TEST_F(AtapiCdromTest, ReadCapacityReportsTheLastLbaAndA2048ByteBlock) {
    MountDisc(1000);
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x25});
    ASSERT_FALSE(CheckCondition());
    auto d = DrainData();
    ASSERT_EQ(d.size(), 8u);
    uint32_t last_lba = (uint32_t(d[0]) << 24) | (uint32_t(d[1]) << 16) | (uint32_t(d[2]) << 8) | d[3];
    uint32_t block = (uint32_t(d[4]) << 24) | (uint32_t(d[5]) << 16) | (uint32_t(d[6]) << 8) | d[7];
    // MMC READ CAPACITY reports the LAST valid LBA, not the block count.
    EXPECT_EQ(last_lba, 999u);
    EXPECT_EQ(block, 2048u) << "a CD-ROM block is genuinely 2048 bytes, not 512";
}

TEST_F(AtapiCdromTest, ReadCapacityFailsWithNoDisc) {
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x25});
    EXPECT_TRUE(CheckCondition());
    Sense s = RequestSense();
    EXPECT_EQ(s.key, 0x02);
    EXPECT_EQ(s.asc, 0x3A);
}

TEST_F(AtapiCdromTest, Read10ReturnsTheRequestedSectors) {
    MountDisc(256);
    SelectDevice0();
    ConsumeResetUnitAttention();
    // READ(10): LBA 17, 2 blocks. Big-endian CDB fields (SPC section 3.4.2).
    SendPacket({0x28, 0, 0, 0, 0, 17, 0, 0, 2, 0});
    ASSERT_FALSE(CheckCondition());
    auto d = DrainData();
    ASSERT_EQ(d.size(), std::size_t(2 * kSectorBytes));
    EXPECT_EQ(d[0], 17) << "first sector stamped with its own LBA";
    EXPECT_EQ(d[std::size_t(kSectorBytes)], 18);
    EXPECT_EQ(IntReason(), 0x03) << "command complete once the data is drained";
}

TEST_F(AtapiCdromTest, Read10OfZeroBlocksSucceedsWithoutData) {
    // A transfer length of zero is explicitly not an error in SCSI.
    MountDisc(16);
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x28, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    EXPECT_FALSE(CheckCondition());
    EXPECT_FALSE(Drq());
}

TEST_F(AtapiCdromTest, Read10PastTheEndOfTheDiscIsAnIllegalRequest) {
    MountDisc(16);
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x28, 0, 0, 0, 0, 15, 0, 0, 4, 0});  // 4 blocks from LBA 15 of 16
    EXPECT_TRUE(CheckCondition());
    Sense s = RequestSense();
    EXPECT_EQ(s.key, 0x05) << "ILLEGAL REQUEST";
    EXPECT_EQ(s.asc, 0x21) << "LOGICAL BLOCK ADDRESS OUT OF RANGE";
}

TEST_F(AtapiCdromTest, ByteCountLimitSplitsTheDataIntoSeparateDrqBlocks) {
    // Real ATAPI PIO hands data over in blocks no larger than the limit the
    // host wrote to the byte-count registers, with an interrupt per block
    // (ATA/ATAPI-4 section 9.6.2). A driver that set a 2048-byte limit and
    // got 6144 bytes in one burst would overrun its own buffer.
    MountDisc(16);
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x28, 0, 0, 0, 0, 0, 0, 0, 3, 0}, /*limit=*/2048);
    ASSERT_TRUE(Drq());
    EXPECT_EQ(ByteCount(), 2048);
    int blocks = 0;
    std::vector<uint8_t> all;
    while (Drq()) {
        ++blocks;
        cd.in(0x177);  // acknowledge this block's interrupt, as a driver would
        EXPECT_FALSE(cd.irq_pending());
        uint16_t count = ByteCount();
        EXPECT_EQ(count, 2048);
        for (uint16_t i = 0; i < count; i += 2) {
            uint16_t w = cd.data_in16();
            all.push_back(uint8_t(w & 0xFF));
            all.push_back(uint8_t(w >> 8));
        }
        EXPECT_TRUE(cd.irq_pending()) << "each new block (and the final completion) interrupts";
    }
    EXPECT_EQ(blocks, 3);
    ASSERT_EQ(all.size(), std::size_t(3 * kSectorBytes));
    EXPECT_EQ(all[0], 0);
    EXPECT_EQ(all[std::size_t(kSectorBytes)], 1);
    EXPECT_EQ(all[std::size_t(2 * kSectorBytes)], 2);
}

TEST_F(AtapiCdromTest, OddByteCountLimitDropsToTheNextEvenValue) {
    // ATA/ATAPI-4 section 7.3.2: an odd byte-count limit is unusable, so the
    // device uses the even value below it -- the classic case being a host
    // that writes 0xFFFF. Here a 2049-byte limit must behave as 2048 rather
    // than handing over an odd-length block.
    MountDisc(8);
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x28, 0, 0, 0, 0, 0, 0, 0, 2, 0}, /*limit=*/2049);
    ASSERT_TRUE(Drq());
    EXPECT_EQ(ByteCount(), 2048);
    EXPECT_EQ(CountDataBlocks(), 2);
}

TEST_F(AtapiCdromTest, Read10IsPacedToRealTwoSpeedDriveTiming) {
    // Realism check, per CLAUDE.md: a 2x CD-ROM moves 307,200 bytes/sec and
    // takes ~250 ms to get its head to a non-sequential request. Reading one
    // sector cannot possibly complete in less than that -- if it does, the
    // device is faster than the hardware it claims to be.
    MountDisc(64);
    SelectDevice0();
    ConsumeResetUnitAttention();

    std::vector<uint8_t> cdb = {0x28, 0, 0, 0, 0, 40, 0, 0, 1, 0, 0, 0};
    cd.out(0x174, 0xFE);
    cd.out(0x175, 0xFF);
    cd.out(0x177, 0xA0);
    for (int i = 0; i < 12; i += 2) {
        cd.data_out16(uint16_t(uint16_t(cdb[std::size_t(i)]) |
                               (uint16_t(cdb[std::size_t(i) + 1]) << 8)));
    }
    ASSERT_TRUE(Busy());

    // 0.25 s access + 2048/307200 s transfer is ~0.2567 s, i.e. ~16.9M
    // cycles of this machine's 66 MHz clock. 100 ms in, nothing may be ready.
    uint64_t c = cycles_;
    c += 6'600'000;
    cd.tick(c);
    EXPECT_TRUE(Busy()) << "completed sooner than a real 2x drive could";
    c += 20'000'000;  // now well past the real access+transfer time
    cd.tick(c);
    EXPECT_FALSE(Busy());
    cycles_ = c;
    ASSERT_TRUE(Drq());
    EXPECT_EQ(cd.data_in16() & 0xFF, 40);
}

// Returns how many cycles a READ(10) of one sector at `lba` takes to
// complete, for comparing the cost of seeks of different lengths.
TEST_F(AtapiCdromTest, ReadAheadBufferServesAShortBackwardsReReadWithNoSeek) {
    // The case that dominates real DOS use of a CD, and the one the old
    // single-position model got badly wrong: re-reading something slightly
    // BEHIND where the last read ended -- a batch file, a utility the batch
    // file runs, an ISO directory extent -- all of which sit within a few
    // tens of KB and are re-read constantly. A real 2x drive holds 64-256KB
    // of read-ahead, so this needs no head movement at all. Charging the full
    // published 250 ms average here made the emulated drive markedly SLOWER
    // than the hardware it models: during a real FreeDOS install 90% of all
    // charged CD time was these penalties (PC486_REVIEW.md §5.8).
    MountDisc(4096);
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x28, 0, 0, 0, 0x02, 0x00, 0, 0, 1, 0});  // LBA 512, cold: pays average
    DrainData();

    // LBA 500 is 13 sectors behind LBA 513 (where the head now is) -- well
    // inside the 32-sector (64KB) read-ahead buffer.
    std::vector<uint8_t> cdb = {0x28, 0, 0, 0, 0x01, 0xF4, 0, 0, 1, 0, 0, 0};
    cd.out(0x174, 0xFE);
    cd.out(0x175, 0xFF);
    cd.out(0x177, 0xA0);
    for (int i = 0; i < 12; i += 2) {
        cd.data_out16(uint16_t(uint16_t(cdb[std::size_t(i)]) |
                               (uint16_t(cdb[std::size_t(i) + 1]) << 8)));
    }
    cycles_ += 1'000'000;  // ~15 ms: past the transfer, nowhere near any seek
    cd.tick(cycles_);
    ASSERT_FALSE(Busy()) << "a read inside the read-ahead buffer must not pay a seek";
    ASSERT_TRUE(Drq());
    EXPECT_EQ(cd.data_in16() & 0xFF, 0xF4);
}

TEST_F(AtapiCdromTest, SeekCostGrowsWithDistanceAndStillRespectsTheShortSeekFloor) {
    // Access time is distance-dependent on real hardware; a datasheet's
    // "average access time" is a one-third-stroke figure, not the cost of
    // every seek. So a short seek must cost meaningfully less than a long
    // one, and both must cost at least the short-seek floor (a sled step plus
    // the CLV spindle-speed change is never free).
    auto cost_cycles = [&](uint32_t from, uint32_t to) {
        MountDisc(30000);
        SelectDevice0();
        ConsumeResetUnitAttention();
        // Park the head via a read at `from` (cold, so this one pays the
        // average -- it is not what is being measured).
        SendPacket({0x28, 0, uint8_t(from >> 24), uint8_t(from >> 16),
                    uint8_t(from >> 8), uint8_t(from), 0, 0, 1, 0});
        DrainData();
        std::vector<uint8_t> cdb = {0x28, 0, uint8_t(to >> 24), uint8_t(to >> 16),
                                    uint8_t(to >> 8), uint8_t(to), 0, 0, 1, 0, 0, 0};
        cd.out(0x174, 0xFE);
        cd.out(0x175, 0xFF);
        cd.out(0x177, 0xA0);
        for (int i = 0; i < 12; i += 2) {
            cd.data_out16(uint16_t(uint16_t(cdb[std::size_t(i)]) |
                                   (uint16_t(cdb[std::size_t(i) + 1]) << 8)));
        }
        uint64_t spent = 0;
        while (Busy() && spent < 100'000'000) {
            cycles_ += 100'000; spent += 100'000; cd.tick(cycles_);
        }
        return spent;
    };
    // 1000 sectors apart on a 30000-sector disc vs. a near-full-stroke move.
    const uint64_t shortish = cost_cycles(1000, 2000);
    const uint64_t longish  = cost_cycles(1000, 29000);
    EXPECT_LT(shortish, longish) << "seek cost must grow with distance";
    // Short-seek floor: kSeekMinSec = 80 ms = 5.28M cycles at 66 MHz, plus
    // transfer. Must not be free, and must not reach the 250 ms average.
    EXPECT_GT(shortish, 5'000'000u) << "even a short seek costs the sled-step floor";
    EXPECT_LT(shortish, 16'500'000u) << "a short seek must cost less than the 250 ms average";
    // A near-full-stroke seek costs more than the average, as on real hardware.
    EXPECT_GT(longish, 16'500'000u);
}

TEST_F(AtapiCdromTest, SequentialReadsSkipTheAccessPenalty) {
    // A read that continues where the last one stopped costs only transfer
    // time: the head is already there and the read-ahead buffer holds the
    // data. This is why DOS software that reads a CD sequentially feels an
    // order of magnitude faster than software that seeks around -- and why
    // modeling a flat per-command access time would be wrong in a way a user
    // would actually feel. (The buffer covers rather more than strictly
    // contiguous reads -- see ReadAheadBufferServesAShortBackwardsReReadWithNoSeek.)
    MountDisc(64);
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x28, 0, 0, 0, 0, 10, 0, 0, 1, 0});  // pays the seek
    DrainData();

    // LBA 11 continues from LBA 10's single block: transfer time only,
    // 2048/307200 s = ~6.7 ms = ~440k cycles at 66 MHz.
    std::vector<uint8_t> cdb = {0x28, 0, 0, 0, 0, 11, 0, 0, 1, 0, 0, 0};
    cd.out(0x174, 0xFE);
    cd.out(0x175, 0xFF);
    cd.out(0x177, 0xA0);
    for (int i = 0; i < 12; i += 2) {
        cd.data_out16(uint16_t(uint16_t(cdb[std::size_t(i)]) |
                               (uint16_t(cdb[std::size_t(i) + 1]) << 8)));
    }
    cycles_ += 1'000'000;  // ~15 ms: past the transfer, nowhere near a 250 ms seek
    cd.tick(cycles_);
    ASSERT_FALSE(Busy()) << "a sequential read must not pay the access penalty";
    ASSERT_TRUE(Drq());
    EXPECT_EQ(cd.data_in16() & 0xFF, 11);
}

TEST_F(AtapiCdromTest, Seek10PositionsTheHeadWithoutReturningData) {
    // MMC SEEK(10). FreeDOS's ATAPICDD.SYS issues this for its own DOS seek
    // device command, so it is a driver path that genuinely gets exercised.
    MountDisc(64);
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x2B, 0, 0, 0, 0, 30, 0, 0, 0, 0});
    EXPECT_FALSE(CheckCondition());
    EXPECT_FALSE(Drq()) << "status only, no data phase";

    // Having sought there, the matching read is already in position.
    std::vector<uint8_t> cdb = {0x28, 0, 0, 0, 0, 30, 0, 0, 1, 0, 0, 0};
    cd.out(0x174, 0xFE);
    cd.out(0x175, 0xFF);
    cd.out(0x177, 0xA0);
    for (int i = 0; i < 12; i += 2) {
        cd.data_out16(uint16_t(uint16_t(cdb[std::size_t(i)]) |
                               (uint16_t(cdb[std::size_t(i) + 1]) << 8)));
    }
    cycles_ += 1'000'000;
    cd.tick(cycles_);
    EXPECT_FALSE(Busy()) << "the seek already paid the access time";
    EXPECT_EQ(cd.data_in16() & 0xFF, 30);
}

TEST_F(AtapiCdromTest, Seek10PastTheEndOfTheDiscIsAnIllegalRequest) {
    MountDisc(16);
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x2B, 0, 0, 0, 0, 99, 0, 0, 0, 0});
    EXPECT_TRUE(CheckCondition());
    Sense s = RequestSense();
    EXPECT_EQ(s.key, 0x05);
    EXPECT_EQ(s.asc, 0x21) << "LOGICAL BLOCK ADDRESS OUT OF RANGE";
}

TEST_F(AtapiCdromTest, GetEventStatusNotificationIsRefusedAsAnachronistic) {
    // GET EVENT STATUS NOTIFICATION (4Ah) is an MMC-2 (1997) command: a
    // 1993-94 2x drive genuinely predates it, so refusing it is the
    // period-accurate answer rather than a gap. FreeDOS's ATAPICDD.SYS asks
    // for it first in getMediaStatus() but handles the failure explicitly
    // (its @@assumeNotSupported path returns STATUS_MEDIA_UNKNOWN), so the
    // driver degrades to polling TEST UNIT READY -- which is what a real
    // period drive forced it to do.
    MountDisc(16);
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x4A, 0x01, 0, 0, 0x10, 0, 0, 0, 8, 0});
    EXPECT_TRUE(CheckCondition());
    Sense s = RequestSense();
    EXPECT_EQ(s.key, 0x05);
    EXPECT_EQ(s.asc, 0x20) << "INVALID COMMAND OPERATION CODE";
}

TEST_F(AtapiCdromTest, ModeSense10ReturnsTheCdCapabilitiesPage) {
    // Page 2Ah, CD Capabilities and Mechanical Status (MMC / SFF-8020i) --
    // what a driver probes for the drive's speed, loader type and whether it
    // can eject. It describes the drive, so it answers with an empty tray.
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x5A, 0, 0x2A, 0, 0, 0, 0, 0, 30, 0});
    ASSERT_FALSE(CheckCondition());
    auto d = DrainData();
    ASSERT_EQ(d.size(), 30u);
    EXPECT_EQ((uint16_t(d[0]) << 8) | d[1], 28) << "mode data length: total minus 2";
    EXPECT_EQ((uint16_t(d[6]) << 8) | d[7], 0) << "no block descriptors";
    const uint8_t *p = &d[8];
    EXPECT_EQ(p[0] & 0x3F, 0x2A) << "page code";
    EXPECT_EQ(p[1], 0x14) << "page length, SFF-8020i/MMC-1 form";
    EXPECT_EQ((p[6] >> 5) & 0x07, 0x01) << "loading mechanism: tray";
    EXPECT_TRUE(p[6] & 0x08) << "eject supported -- pairs with START STOP UNIT";
    EXPECT_TRUE(p[6] & 0x01) << "lock supported -- pairs with PREVENT ALLOW MEDIUM REMOVAL";
    EXPECT_EQ((uint16_t(p[8]) << 8) | p[9], 353) << "2x = 2 x 176.4 KB/s";
    EXPECT_EQ((uint16_t(p[14]) << 8) | p[15], 353) << "current read speed";
}

TEST_F(AtapiCdromTest, ModeSense10RejectsAnUnsupportedPage) {
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x5A, 0, 0x0D, 0, 0, 0, 0, 0, 30, 0});
    EXPECT_TRUE(CheckCondition());
    Sense s = RequestSense();
    EXPECT_EQ(s.key, 0x05) << "ILLEGAL REQUEST";
    EXPECT_EQ(s.asc, 0x24) << "INVALID FIELD IN CDB";
}

TEST_F(AtapiCdromTest, ModeSenseSixByteFormIsNotPartOfTheAtapiCommandSet) {
    // A real quirk worth keeping: SFF-8020i defines only the 10-byte MODE
    // SENSE/MODE SELECT, so a real ATAPI CD-ROM rejects the 6-byte form with
    // INVALID COMMAND OPERATION CODE and drivers probe with it precisely to
    // learn they must use the 10-byte form.
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x1A, 0, 0x2A, 0, 30, 0});
    EXPECT_TRUE(CheckCondition());
    EXPECT_TRUE(cd.in(0x171) & 0x04) << "ABRT alongside the sense key";
    Sense s = RequestSense();
    EXPECT_EQ(s.key, 0x05);
    EXPECT_EQ(s.asc, 0x20) << "INVALID COMMAND OPERATION CODE";
}

TEST_F(AtapiCdromTest, ReadTocReportsOneDataTrackAndTheLeadOut) {
    MountDisc(500);
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x43, 0, 0, 0, 0, 0, 0, 0, 20, 0});
    ASSERT_FALSE(CheckCondition());
    auto d = DrainData();
    ASSERT_EQ(d.size(), 20u);
    EXPECT_EQ((uint16_t(d[0]) << 8) | d[1], 18) << "TOC data length";
    EXPECT_EQ(d[2], 1) << "first track";
    EXPECT_EQ(d[3], 1) << "last track";
    EXPECT_EQ(d[5], 0x14) << "ADR=1, CONTROL=4: a data track";
    EXPECT_EQ(d[6], 1) << "track number";
    uint32_t track_lba = (uint32_t(d[8]) << 24) | (uint32_t(d[9]) << 16) | (uint32_t(d[10]) << 8) | d[11];
    EXPECT_EQ(track_lba, 0u);
    EXPECT_EQ(d[14], 0xAA) << "the lead-out is reported as pseudo-track AAh";
    uint32_t leadout = (uint32_t(d[16]) << 24) | (uint32_t(d[17]) << 16) | (uint32_t(d[18]) << 8) | d[19];
    EXPECT_EQ(leadout, 500u);
}

TEST_F(AtapiCdromTest, ReadTocInMsfFormCarriesTheRedBookTwoSecondPregap) {
    // MSF addresses are offset by 150 frames: LBA 0 is at 00:02:00, not
    // 00:00:00. A driver that hands an MSF address straight back as an LBA
    // without removing the pregap reads 150 sectors past where it meant to.
    MountDisc(500);
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x43, 0x02, 0, 0, 0, 0, 0, 0, 20, 0});
    auto d = DrainData();
    ASSERT_EQ(d.size(), 20u);
    EXPECT_EQ(d[9], 0) << "minutes";
    EXPECT_EQ(d[10], 2) << "seconds";
    EXPECT_EQ(d[11], 0) << "frames";
    // Lead-out at LBA 500 -> frame 650 -> 00:08:50.
    EXPECT_EQ(d[17], 0);
    EXPECT_EQ(d[18], 8);
    EXPECT_EQ(d[19], 50);
}

TEST_F(AtapiCdromTest, ReadTocFormatOneReportsSessionInformation) {
    // Format 0001b ("Session Information") is what FreeDOS's UDVD2.SYS uses
    // as its disc-present check -- it issues `43 00 01 00 00 00 00 00 0C 00`
    // verbatim, i.e. format 1 with a 12-byte allocation length. Refusing it
    // with INVALID FIELD IN CDB (as this device originally did, having been
    // written against ATAPICDD.SYS instead) made the entire drive read back
    // as "drive not ready" to DOS, so nothing on the CD was reachable at all.
    // See PC486_REVIEW.md §5.5.
    MountDisc(500);
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x43, 0x00, 0x01, 0, 0, 0, 0, 0, 0x0C, 0});
    ASSERT_FALSE(CheckCondition());
    auto d = DrainData();
    ASSERT_EQ(d.size(), 12u) << "a 4-byte header plus exactly one track descriptor";
    EXPECT_EQ((uint16_t(d[0]) << 8) | d[1], 10) << "TOC data length: total minus this field";
    EXPECT_EQ(d[2], 1) << "first complete session";
    EXPECT_EQ(d[3], 1) << "last complete session -- these images are single-session";
    EXPECT_EQ(d[5], 0x14) << "ADR=1, CONTROL=4: a data track";
    EXPECT_EQ(d[6], 1) << "first track number in the last complete session";
    uint32_t lba = (uint32_t(d[8]) << 24) | (uint32_t(d[9]) << 16) | (uint32_t(d[10]) << 8) | d[11];
    EXPECT_EQ(lba, 0u) << "that track starts at LBA 0";
}

TEST_F(AtapiCdromTest, ReadTocStillRejectsAnUnimplementedFormat) {
    // Format 1 being accepted must not turn READ TOC into a command that
    // accepts anything: format 2 (Full TOC) and up are genuinely not
    // implemented, and a driver probing for them is supposed to learn that.
    MountDisc(500);
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x43, 0x00, 0x02, 0, 0, 0, 0, 0, 0x0C, 0});
    ASSERT_TRUE(CheckCondition());
    auto s = RequestSense();
    EXPECT_EQ(s.key, 0x05) << "ILLEGAL REQUEST";
    EXPECT_EQ(s.asc, 0x24) << "INVALID FIELD IN CDB";
}

TEST_F(AtapiCdromTest, StartStopUnitEjectsAndTheMediaChangeIsReported) {
    // The path a DOS CD-ROM driver's "did the disc change?" poll actually
    // rides on (SPC section 5.6): the change raises a unit attention that
    // the next command reports as CHECK CONDITION / 06h / 28h 00h, and it is
    // cleared once reported.
    MountDisc(64);
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x00});
    ASSERT_FALSE(CheckCondition());

    SendPacket({0x1B, 0, 0, 0, 0x02});  // START STOP UNIT: LoEj=1, Start=0 -> eject
    EXPECT_FALSE(CheckCondition()) << "the eject itself succeeds";
    EXPECT_FALSE(cd.media_present());

    SendPacket({0x00});
    EXPECT_TRUE(CheckCondition());
    Sense s = RequestSense();
    EXPECT_EQ(s.key, 0x06) << "UNIT ATTENTION";
    EXPECT_EQ(s.asc, 0x28) << "NOT READY TO READY CHANGE, MEDIUM MAY HAVE CHANGED";

    // With the change reported, the standing condition is simply "no disc".
    SendPacket({0x00});
    EXPECT_TRUE(CheckCondition());
    Sense s2 = RequestSense();
    EXPECT_EQ(s2.key, 0x02);
    EXPECT_EQ(s2.asc, 0x3A);

    // Re-inserting raises the change again, and reads work afterward.
    MountDisc(64);
    SendPacket({0x00});
    EXPECT_TRUE(CheckCondition());
    Sense s3 = RequestSense();
    EXPECT_EQ(s3.key, 0x06);
    EXPECT_EQ(s3.asc, 0x28);
    SendPacket({0x28, 0, 0, 0, 0, 5, 0, 0, 1, 0});
    ASSERT_FALSE(CheckCondition());
    auto d = DrainData();
    ASSERT_EQ(d.size(), std::size_t(kSectorBytes));
    EXPECT_EQ(d[0], 5);
}

TEST_F(AtapiCdromTest, PreventMediumRemovalRefusesTheEjectCommand) {
    // SPC section 7.12: with removal prevented, a drive refuses the eject
    // rather than obeying it -- what stops a program from ejecting a disc it
    // is still reading from.
    MountDisc(16);
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x1E, 0, 0, 0, 0x01});  // PREVENT ALLOW MEDIUM REMOVAL: prevent
    ASSERT_FALSE(CheckCondition());

    SendPacket({0x1B, 0, 0, 0, 0x02});  // eject attempt
    EXPECT_TRUE(CheckCondition());
    EXPECT_TRUE(cd.media_present()) << "the tray stayed shut";
    Sense s = RequestSense();
    EXPECT_EQ(s.key, 0x05) << "ILLEGAL REQUEST";
    EXPECT_EQ(s.asc, 0x53) << "MEDIA REMOVAL PREVENTED";
    EXPECT_EQ(s.ascq, 0x02);

    SendPacket({0x1E, 0, 0, 0, 0x00});  // allow again
    SendPacket({0x1B, 0, 0, 0, 0x02});
    EXPECT_FALSE(cd.media_present());
}

TEST_F(AtapiCdromTest, HostEjectOverridesTheDriverLock) {
    // The front-end eject button is the user physically taking the disc --
    // the real-world equivalent of the emergency eject hole, which a
    // software lock cannot stop. The CDB path above honors the lock; this
    // one deliberately does not.
    MountDisc(16);
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x1E, 0, 0, 0, 0x01});
    cd.eject();
    EXPECT_FALSE(cd.media_present());
}

TEST_F(AtapiCdromTest, NienMasksTheCompletionInterrupt) {
    // Device Control bit 1 (nIEN) masks INTRQ to the host, same as the
    // primary channel's.
    MountDisc(16);
    SelectDevice0();
    ConsumeResetUnitAttention();
    cd.in(0x177);         // acknowledge anything outstanding
    cd.out(0x376, 0x02);  // nIEN
    SendPacket({0x00});
    EXPECT_FALSE(cd.irq_pending());
    cd.out(0x376, 0x00);
    SendPacket({0x00});
    EXPECT_TRUE(cd.irq_pending());
}

TEST_F(AtapiCdromTest, BusyReflectsAnInFlightCommand) {
    MountDisc(16);
    SelectDevice0();
    ConsumeResetUnitAttention();
    EXPECT_FALSE(cd.busy());
    cd.out(0x174, 0xFE);
    cd.out(0x175, 0xFF);
    cd.out(0x177, 0xA0);
    std::vector<uint8_t> cdb = {0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0};
    for (int i = 0; i < 12; i += 2) {
        cd.data_out16(uint16_t(uint16_t(cdb[std::size_t(i)]) |
                               (uint16_t(cdb[std::size_t(i) + 1]) << 8)));
    }
    EXPECT_TRUE(cd.busy());
    RunToIdle();
    EXPECT_FALSE(cd.busy());
}

TEST_F(AtapiCdromTest, DiscSurvivesAReset) {
    // A reset does not open the tray -- the disc is still in the drive.
    MountDisc(16);
    cd.reset();
    EXPECT_TRUE(cd.media_present());
}

TEST_F(AtapiCdromTest, MountTruncatesAPartialTrailingSector) {
    // A real drive addresses whole 2048-byte blocks and cannot read a
    // partial one, so a ragged image is rounded down rather than exposing a
    // short final sector.
    std::vector<uint8_t> img(std::size_t(3 * kSectorBytes) + 7, 0xAB);
    cd.mount(img.data(), img.size());
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x25});
    auto d = DrainData();
    ASSERT_EQ(d.size(), 8u);
    EXPECT_EQ(d[3], 2) << "last LBA = 2, i.e. exactly 3 whole blocks";
}

}  // namespace
