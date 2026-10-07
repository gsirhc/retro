// GoogleTest suite for the secondary-channel ATAPI CD-ROM (SFF-8020i / MMC).
// Register access goes through in()/out()/data_in16()/data_out16() like the chipset decode.

#include <gtest/gtest.h>

#include "atapi_cdrom.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace {

using pc486::AtapiCdrom;

constexpr int kSectorBytes = AtapiCdrom::kBytesPerSector;
constexpr int kFramesPerLba = 588;  // CD-DA stereo sample pairs per 2352-byte frame

// ISO of `blocks` 2048-byte sectors, each stamped with its LBA in the first two bytes.
std::vector<uint8_t> MakeIso(uint32_t blocks) {
    std::vector<uint8_t> img(std::size_t(blocks) * kSectorBytes, 0);
    for (uint32_t b = 0; b < blocks; ++b) {
        img[std::size_t(b) * kSectorBytes] = uint8_t(b & 0xFF);
        img[std::size_t(b) * kSectorBytes + 1] = uint8_t((b >> 8) & 0xFF);
    }
    return img;
}

std::string MsfString(uint32_t lba) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u", lba / (75 * 60), (lba / 75) % 60, lba % 75);
    return std::string(buf);
}

// Mixed-mode CUE+BIN: a MakeIso() data track, then `audio_lbas` CD-DA frames.
// Left sample is the frame index, right is its complement.
struct CueBin {
    std::string cue;
    std::vector<uint8_t> bin;
};
CueBin MakeCueBin(uint32_t data_blocks, uint32_t audio_lbas) {
    CueBin out;
    out.bin = MakeIso(data_blocks);
    const uint32_t frames = audio_lbas * uint32_t(kFramesPerLba);
    out.bin.reserve(out.bin.size() + std::size_t(frames) * 4);
    for (uint32_t f = 0; f < frames; ++f) {
        const uint16_t l = uint16_t(f);
        const uint16_t r = uint16_t(~f);
        out.bin.push_back(uint8_t(l & 0xFF));
        out.bin.push_back(uint8_t(l >> 8));
        out.bin.push_back(uint8_t(r & 0xFF));
        out.bin.push_back(uint8_t(r >> 8));
    }
    out.cue = "FILE \"disc.bin\" BINARY\n"
              "  TRACK 01 MODE1/2048\n"
              "    INDEX 01 00:00:00\n"
              "  TRACK 02 AUDIO\n"
              "    INDEX 01 " + MsfString(data_blocks) + "\n";
    return out;
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

    // Poll BSY; DRDY is never valid on a packet device.
    void RunToIdle() {
        for (int i = 0; i < 100000 && Busy(); ++i) {
            cycles_ += 100000;
            cd.tick(cycles_);
        }
        ASSERT_FALSE(Busy()) << "command never completed";
    }

    // One PACKET command: byte-count limit, 0xA0, then the 12-byte CDB via the data register.
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

    // Drain every data block, honoring each block's byte count.
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

    // First command after reset reports a unit attention (SPC 5.6); consume it.
    void ConsumeResetUnitAttention() {
        SendPacket({0x00});  // TEST UNIT READY
        RequestSense();
    }

    void MountDisc(uint32_t blocks) {
        auto img = MakeIso(blocks);
        cd.mount(img.data(), img.size());
    }

    void MountCueBin(const CueBin &cb) {
        ASSERT_TRUE(cd.mount_cue(cb.cue.c_str(), cb.bin.data(), cb.bin.size()));
    }

    // Data-OUT PACKET (MODE SELECT(10)). The CDB's Parameter List Length must match param_list.size().
    void SendPacketOut(std::vector<uint8_t> cdb, std::vector<uint8_t> param_list,
                        uint16_t limit = 0xFFFE) {
        cdb.resize(12, 0);
        cd.out(0x174, uint8_t(limit & 0xFF));
        cd.out(0x175, uint8_t(limit >> 8));
        cd.out(0x171, 0x00);
        cd.out(0x177, 0xA0);
        ASSERT_TRUE(Drq());
        for (int i = 0; i < 12; i += 2) {
            cd.data_out16(uint16_t(uint16_t(cdb[std::size_t(i)]) |
                                   (uint16_t(cdb[std::size_t(i) + 1]) << 8)));
        }
        if (param_list.empty()) { RunToIdle(); return; }
        ASSERT_TRUE(Drq()) << "device must request the parameter list via DRQ";
        ASSERT_EQ(IntReason(), 0x00) << "C/D=0, I/O=0: data from host to device";
        param_list.resize((param_list.size() + 1) & ~std::size_t(1), 0);
        for (std::size_t i = 0; i < param_list.size(); i += 2) {
            cd.data_out16(uint16_t(uint16_t(param_list[i]) | (uint16_t(param_list[i + 1]) << 8)));
        }
        RunToIdle();
    }

    // SFF-8020i Table 60 Audio Control page.
    static std::vector<uint8_t> AudioControlPage(
        std::initializer_list<std::pair<uint8_t, uint8_t>> ports) {
        std::vector<uint8_t> p(16, 0);
        p[0] = 0x0E;
        p[1] = 0x0E;
        int i = 0;
        for (auto &port : ports) {
            p[8 + i * 2] = port.first;
            p[9 + i * 2] = port.second;
            ++i;
        }
        return p;
    }

    // MODE SELECT(10) CDB: PF=1, length = 8-byte header + 16-byte page.
    static std::vector<uint8_t> ModeSelectCdb(uint16_t param_len) {
        std::vector<uint8_t> cdb(12, 0);
        cdb[0] = 0x55;
        cdb[1] = 0x10;  // PF = 1
        cdb[7] = uint8_t(param_len >> 8);
        cdb[8] = uint8_t(param_len & 0xFF);
        return cdb;
    }

    // Tick until the CD-DA engine has produced `want` samples.
    std::vector<AtapiCdrom::Sample> RunAudioUntil(std::size_t want) {
        std::vector<AtapiCdrom::Sample> all;
        for (int i = 0; i < 200000 && all.size() < want; ++i) {
            cycles_ += 64;  // small steps: 44.1kHz at 66MHz is ~1496 cycles/sample
            cd.tick(cycles_);
            auto drained = cd.drain_samples();
            all.insert(all.end(), drained.begin(), drained.end());
        }
        return all;
    }

    // Tick until the drive reports playback is no longer in progress.
    void RunAudioUntilStopped() {
        for (int i = 0; i < 200000 && cd.playing_audio(); ++i) {
            cycles_ += 64;
            cd.tick(cycles_);
        }
        ASSERT_FALSE(cd.playing_audio()) << "playback never stopped";
    }
};

TEST_F(AtapiCdromTest, OwnsSecondaryChannelPortsOnly) {
    for (uint16_t p = 0x170; p <= 0x177; ++p) EXPECT_TRUE(cd.owns(p)) << std::hex << p;
    EXPECT_TRUE(cd.owns(0x376));
    // The primary channel and floppy 0x377 stay out of this device's decode.
    EXPECT_FALSE(cd.owns(0x1F0));
    EXPECT_FALSE(cd.owns(0x3F6));
    EXPECT_FALSE(cd.owns(0x377));
    EXPECT_FALSE(cd.owns(0x16F));
    EXPECT_FALSE(cd.owns(0x178));
}

TEST_F(AtapiCdromTest, AtapiSignatureAfterReset) {
    // ATA/ATAPI-4 9.1: a packet device signs Sector Count/Number 01h, Cyl Low/High 14h/EBh.
    EXPECT_EQ(cd.in(0x172), 0x01);
    EXPECT_EQ(cd.in(0x173), 0x01);
    EXPECT_EQ(cd.in(0x174), 0x14);
    EXPECT_EQ(cd.in(0x175), 0xEB);
    // Status reads 00h: DRDY is not set on a packet device.
    EXPECT_EQ(cd.in(0x376), 0x00);
    EXPECT_EQ(cd.in(0x171), 0x01);  // post-reset "diagnostics passed" error code
}

TEST_F(AtapiCdromTest, ScratchRegisterProbeReadsBackThroughTheSharedLatches) {
    // Offset 2 is one latch (Sector Count written, Interrupt Reason read), so the BIOS 0x55/0xAA read-back works.
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
    // ATA/ATAPI-6 Table 18: with device 1 selected, a packet device returns 00h for the task file and status.
    // ATAPICDD.SYS probes device 1 by read-back and would otherwise find a phantom slave.
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
    // Table 18: Error still reports device 0's contents.
    EXPECT_EQ(cd.in(0x171), 0x01);

    // Selecting device 0 restores its signature.
    SelectDevice0();
    EXPECT_EQ(cd.in(0x172), 0x01);
    EXPECT_EQ(cd.in(0x173), 0x01);
    EXPECT_EQ(cd.in(0x174), 0x14);
    EXPECT_EQ(cd.in(0x175), 0xEB);
}

TEST_F(AtapiCdromTest, ExecuteDeviceDiagnosticIsTheOneCommandTheAbsentDeviceOneAnswers) {
    // Table 18 command row: only EXECUTE DEVICE DIAGNOSTIC is answered for device 1.
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
    // Only command writes are gated on selection; there is no device 1 state machine.
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

    // Word 0 differs from ATA IDENTIFY DEVICE (ATA/ATAPI-4 8.13.8, Table 12).
    EXPECT_EQ(w[0] & 0xC000, 0x8000) << "bits 15:14 = 10b: ATAPI device";
    EXPECT_EQ((w[0] >> 8) & 0x1F, 0x05) << "CD-ROM command set (SCSI-3 MMC)";
    EXPECT_TRUE(w[0] & 0x0080) << "removable media";
    EXPECT_EQ(w[0] & 0x0003, 0x0000) << "12-byte command packet";
    EXPECT_EQ((w[0] >> 5) & 0x03, 0x02) << "accelerated DRQ -- must agree with "
                                           "begin_packet() not raising an IRQ "
                                           "for the packet-request phase";

    // Word 49: LBA yes, DMA not advertised (PIO only).
    EXPECT_TRUE(w[49] & 0x0200) << "LBA supported";
    EXPECT_FALSE(w[49] & 0x0100) << "DMA must not be advertised";
    EXPECT_EQ(w[63], 0x0000) << "no multiword DMA modes";
    EXPECT_TRUE(w[82] & 0x0010) << "PACKET command feature set supported";
    EXPECT_TRUE(w[82] & 0x0200) << "DEVICE RESET supported";
    EXPECT_TRUE(w[83] & 0x4000) << "words 82-84 marked valid";

    // Words 27-46: ATA strings byte-swap each pair (ATA/ATAPI-4 8.12.8).
    std::string model;
    for (int i = 27; i <= 46; ++i) {
        model.push_back(char(w[std::size_t(i)] >> 8));
        model.push_back(char(w[std::size_t(i)] & 0xFF));
    }
    EXPECT_EQ(model.substr(0, 18), "RETROWEB CD-ROM 2X");
}

TEST_F(AtapiCdromTest, IdentifyDeviceIsAbortedWithTheAtapiSignature) {
    // ATA/ATAPI-4 8.12.1: a packet device aborts IDENTIFY DEVICE (0xEC) and places its signature.
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
    // ATA/ATAPI-4 8.7: DEVICE RESET reasserts the signature and does not assert INTRQ.
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
    // SPC 5.6: reset raises a unit attention, reported once as CHECK CONDITION.
    MountDisc(64);
    cd.reset();
    SelectDevice0();
    SendPacket({0x00});  // TEST UNIT READY
    EXPECT_TRUE(CheckCondition());
    Sense s = RequestSense();
    EXPECT_EQ(s.key, 0x06) << "UNIT ATTENTION";
    EXPECT_EQ(s.asc, 0x29) << "POWER ON, RESET, OR BUS DEVICE RESET OCCURRED";
    EXPECT_EQ(s.ascq, 0x00);

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
    // REQUEST SENSE works with no disc; reading clears the sense (SPC 7.20).
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
    // INQUIRY works with an empty tray and is exempt from unit attention (SPC 5.6).
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
    // PIO blocks never exceed the host's byte-count limit, one interrupt each (ATA/ATAPI-4 9.6.2).
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
    // ATA/ATAPI-4 7.3.2: an odd byte-count limit rounds down to even.
    MountDisc(8);
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x28, 0, 0, 0, 0, 0, 0, 0, 2, 0}, /*limit=*/2049);
    ASSERT_TRUE(Drq());
    EXPECT_EQ(ByteCount(), 2048);
    EXPECT_EQ(CountDataBlocks(), 2);
}

TEST_F(AtapiCdromTest, Read10IsPacedToRealTwoSpeedDriveTiming) {
    // 2x CD-ROM: 307,200 B/s and ~250 ms access, so one sector cannot finish sooner.
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

    // 0.25 s + 2048/307200 s is ~16.9M cycles at 66 MHz.
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

// Cycles for a READ(10) of one sector at `lba`.
TEST_F(AtapiCdromTest, ReadAheadBufferServesAShortBackwardsReReadWithNoSeek) {
    // Re-reading just behind the head hits the 64-256KB read-ahead buffer, so no seek (PC486_REVIEW.md §5.8).
    MountDisc(4096);
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x28, 0, 0, 0, 0x02, 0x00, 0, 0, 1, 0});  // LBA 512, cold: pays average
    DrainData();

    // LBA 500 is 13 sectors behind the head, inside the 32-sector read-ahead.
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
    // Seek time scales with distance: short seeks cost less than long ones but never zero.
    auto cost_cycles = [&](uint32_t from, uint32_t to) {
        MountDisc(30000);
        SelectDevice0();
        ConsumeResetUnitAttention();
        // Park the head with a cold read (pays the average, not measured).
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
    // Short-seek floor kSeekMinSec = 80 ms = 5.28M cycles at 66 MHz, plus transfer.
    EXPECT_GT(shortish, 5'000'000u) << "even a short seek costs the sled-step floor";
    EXPECT_LT(shortish, 16'500'000u) << "a short seek must cost less than the 250 ms average";
    // A near-full-stroke seek costs more than the average, as on real hardware.
    EXPECT_GT(longish, 16'500'000u);
}

TEST_F(AtapiCdromTest, SequentialReadsSkipTheAccessPenalty) {
    // A sequential read costs transfer time only: head in place, read-ahead holds the data.
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
    // MMC SEEK(10), used by ATAPICDD.SYS for its seek device command.
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
    // GET EVENT STATUS NOTIFICATION is MMC-2 (1997). A 1993-94 2x drive refuses it and
    // ATAPICDD.SYS falls back to polling TEST UNIT READY.
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
    // Page 2Ah CD Capabilities and Mechanical Status (SFF-8020i); works with an empty tray.
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x5A, 0, 0x2A, 0, 0, 0, 0, 0, 30, 0});
    ASSERT_FALSE(CheckCondition());
    auto d = DrainData();
    ASSERT_EQ(d.size(), 28u);
    EXPECT_EQ((uint16_t(d[0]) << 8) | d[1], 26) << "mode data length: total minus 2";
    EXPECT_EQ((uint16_t(d[6]) << 8) | d[7], 0) << "no block descriptors";
    const uint8_t *p = &d[8];
    EXPECT_EQ(p[0] & 0x3F, 0x2A) << "page code";
    EXPECT_EQ(p[1], 0x12) << "page length, SFF-8020i Table 68: 18 more bytes, 20-byte page";
    EXPECT_TRUE(p[4] & 0x01) << "Audio Play -- PLAY AUDIO(10)/MSF are implemented";
    EXPECT_EQ((p[6] >> 5) & 0x07, 0x01) << "loading mechanism: tray";
    EXPECT_TRUE(p[6] & 0x08) << "eject supported -- pairs with START STOP UNIT";
    EXPECT_TRUE(p[6] & 0x01) << "lock supported -- pairs with PREVENT ALLOW MEDIUM REMOVAL";
    EXPECT_TRUE(p[7] & 0x01) << "separate volume -- mode page 0Eh's 4 independent ports";
    EXPECT_TRUE(p[7] & 0x02) << "separate channel mute -- ditto";
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
    // SFF-8020i defines only the 10-byte MODE SENSE/SELECT; the 6-byte form gets INVALID COMMAND OPERATION CODE.
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
    // MSF is offset by 150 frames: LBA 0 is 00:02:00.
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
    // Format 0001b (Session Information) is UDVD2.SYS's disc-present check (PC486_REVIEW.md §5.5).
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
    // Full TOC (format 2) and up stay unimplemented.
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
    // Disc change raises a unit attention: CHECK CONDITION / 06h / 28h 00h, once (SPC 5.6).
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
    // SPC 7.12: with removal prevented, eject is refused.
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
    // The front-end eject button bypasses the software lock, like the emergency eject hole.
    MountDisc(16);
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x1E, 0, 0, 0, 0x01});
    cd.eject();
    EXPECT_FALSE(cd.media_present());
}

TEST_F(AtapiCdromTest, NienMasksTheCompletionInterrupt) {
    // nIEN masks INTRQ.
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

// --- CD-DA audio playback ---------------------------------------------

TEST_F(AtapiCdromTest, PlayAudio10PlaysTheAudioTrackAtTheRealSampleRate) {
    MountCueBin(MakeCueBin(/*data_blocks=*/4, /*audio_lbas=*/2));
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x45, 0, 0, 0, 0, 4, 0, 0, 2});  // PLAY AUDIO(10), LBA 4, 2 blocks
    EXPECT_TRUE(cd.playing_audio());
    auto samples = RunAudioUntil(10);
    ASSERT_GE(samples.size(), 10u);
    for (std::size_t i = 0; i < 10; ++i) {
        EXPECT_EQ(samples[i].left, int16_t(uint16_t(i))) << "frame " << i;
        EXPECT_EQ(samples[i].right, int16_t(~uint16_t(i))) << "frame " << i;
    }
    EXPECT_LT(samples[0].cpu_cycle, samples[9].cpu_cycle) << "timestamps advance with playback";
}

TEST_F(AtapiCdromTest, PlaybackCompletesAtTheEndOfTheRequestedRange) {
    MountCueBin(MakeCueBin(4, /*audio_lbas=*/1));  // exactly 588 frames to play
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x45, 0, 0, 0, 0, 4, 0, 0, 1});
    auto samples = RunAudioUntil(kFramesPerLba);
    RunAudioUntilStopped();
    EXPECT_EQ(samples.size(), std::size_t(kFramesPerLba)) << "no samples past the requested range";
    EXPECT_FALSE(cd.playing_audio());
    SendPacket({0x42, 0, 0x40, 0x01, 0, 0, 0, 0, 16, 0});  // READ SUB-CHANNEL, format 01h
    auto d = DrainData();
    ASSERT_GE(d.size(), 2u);
    EXPECT_EQ(d[1], 0x13) << "MMC Table 116: play operation successfully completed";
}

TEST_F(AtapiCdromTest, PauseHaltsSampleProductionAndResumeContinuesIt) {
    MountCueBin(MakeCueBin(4, 4));
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x45, 0, 0, 0, 0, 4, 0, 0, 4});
    (void)RunAudioUntil(5);
    SendPacket({0x4B, 0, 0, 0, 0, 0, 0, 0, 0});  // PAUSE (Resume bit clear)
    SendPacket({0x42, 0, 0x40, 0x01, 0, 0, 0, 0, 16, 0});
    EXPECT_EQ(DrainData()[1], 0x12) << "paused";
    auto while_paused = RunAudioUntil(5);
    EXPECT_TRUE(while_paused.empty()) << "a paused drive produces no samples";
    SendPacket({0x4B, 0, 0, 0, 0, 0, 0, 0, 0x01});  // RESUME
    auto after_resume = RunAudioUntil(5);
    EXPECT_FALSE(after_resume.empty());
    SendPacket({0x42, 0, 0x40, 0x01, 0, 0, 0, 0, 16, 0});
    EXPECT_EQ(DrainData()[1], 0x11) << "playing";
}

TEST_F(AtapiCdromTest, PauseWithNoPlayInProgressIsAnAbortedCommand) {
    MountCueBin(MakeCueBin(4, 2));
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x4B, 0, 0, 0, 0, 0, 0, 0, 0});
    EXPECT_TRUE(CheckCondition());
    Sense s = RequestSense();
    EXPECT_EQ(s.key, 0x0B) << "ABORTED COMMAND";
    EXPECT_EQ(s.asc, 0xB9) << "PLAY OPERATION ABORTED";
}

TEST_F(AtapiCdromTest, StopPlayScanEndsPlaybackWithNoCurrentAudioStatus) {
    MountCueBin(MakeCueBin(4, 4));
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x45, 0, 0, 0, 0, 4, 0, 0, 4});
    SendPacket({0x4E});  // STOP PLAY/SCAN
    EXPECT_FALSE(cd.playing_audio());
    SendPacket({0x42, 0, 0x40, 0x01, 0, 0, 0, 0, 16, 0});
    EXPECT_EQ(DrainData()[1], 0x15) << "no current audio status";
}

TEST_F(AtapiCdromTest, PlayAudioOnADataTrackLbaIsIllegalModeForThisTrack) {
    MountCueBin(MakeCueBin(4, 2));
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x45, 0, 0, 0, 0, 0, 0, 0, 1});  // LBA 0 is the data track
    EXPECT_TRUE(CheckCondition());
    Sense s = RequestSense();
    EXPECT_EQ(s.key, 0x05);
    EXPECT_EQ(s.asc, 0x64) << "ILLEGAL MODE FOR THIS TRACK OR INCOMPATIBLE MEDIUM";
}

TEST_F(AtapiCdromTest, PlayAudioPastTheEndOfTheDiscIsEndOfUserArea) {
    MountCueBin(MakeCueBin(4, 2));  // disc is 6 LBAs total (4 data + 2 audio)
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x45, 0, 0, 0, 0, 4, 0, 0, 5});  // LBA 4, 5 blocks -> runs to LBA 9
    EXPECT_TRUE(CheckCondition());
    Sense s = RequestSense();
    EXPECT_EQ(s.key, 0x05);
    EXPECT_EQ(s.asc, 0x63) << "END OF USER AREA ENCOUNTERED ON THIS TRACK";
}

TEST_F(AtapiCdromTest, ReadSubChannelReportsTheAudioTrackAndPosition) {
    MountCueBin(MakeCueBin(4, 2));
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x45, 0, 0, 0, 0, 4, 0, 0, 2});
    SendPacket({0x42, 0, 0x40, 0x01, 0, 0, 0, 0, 16, 0});
    auto d = DrainData();
    ASSERT_GE(d.size(), 16u);
    EXPECT_EQ(d[1], 0x11) << "playing";
    EXPECT_EQ(d[5] & 0x04, 0) << "CONTROL: audio track, not data";
    EXPECT_EQ(d[6], 2) << "track number 2, the audio track";
    EXPECT_EQ((uint32_t(d[8]) << 24) | (uint32_t(d[9]) << 16) | (uint32_t(d[10]) << 8) | d[11], 4u)
        << "absolute address: just started, at LBA 4";
    EXPECT_EQ((uint32_t(d[12]) << 24) | (uint32_t(d[13]) << 16) | (uint32_t(d[14]) << 8) | d[15], 0u)
        << "track-relative address: 0 blocks into the track";
}

TEST_F(AtapiCdromTest, ReadTocReportsRealControlBitsForAMixedModeDisc) {
    MountCueBin(MakeCueBin(4, 2));
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x43, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0});
    auto d = DrainData();
    ASSERT_EQ(d.size(), 4u + 2 * 8 + 8);
    EXPECT_EQ(d[2], 1) << "first track";
    EXPECT_EQ(d[3], 2) << "last track";
    EXPECT_EQ(d[5], 0x14) << "track 1 CONTROL: data";
    EXPECT_EQ(d[6], 1);
    EXPECT_EQ(d[13], 0x10) << "track 2 CONTROL: audio";
    EXPECT_EQ(d[14], 2);
    EXPECT_EQ(d[22], 0xAA) << "lead-out pseudo-track";
    EXPECT_EQ((uint32_t(d[24]) << 24) | (uint32_t(d[25]) << 16) | (uint32_t(d[26]) << 8) | d[27], 6u)
        << "lead-out at the whole disc's end, not just the data track's";
}

TEST_F(AtapiCdromTest, DefaultAudioPortsRouteBothChannelsAtFullVolume) {
    // SFF-8020i Table 60: ports 0/1 default to FFh, so audio plays without MODE SELECT.
    MountCueBin(MakeCueBin(4, 2));
    SelectDevice0();
    ConsumeResetUnitAttention();
    SendPacket({0x5A, 0, 0x0E, 0, 0, 0, 0, 0, 30, 0});
    auto d = DrainData();
    ASSERT_GE(d.size(), 8u + 16u);
    const uint8_t *p = &d[8];
    EXPECT_EQ(p[8] & 0x0F, 0x01) << "port 0 <- channel 0 (left)";
    EXPECT_EQ(p[9], 0xFF);
    EXPECT_EQ(p[10] & 0x0F, 0x02) << "port 1 <- channel 1 (right)";
    EXPECT_EQ(p[11], 0xFF);
    EXPECT_EQ(p[13], 0x00) << "port 2: optional, defaults muted";
    EXPECT_EQ(p[15], 0x00) << "port 3: ditto";
}

TEST_F(AtapiCdromTest, ModeSelectAudioControlPageSetsPortsAndReadsBack) {
    MountCueBin(MakeCueBin(4, 2));
    SelectDevice0();
    ConsumeResetUnitAttention();
    auto page = AudioControlPage({{0x02, 0x80}, {0x01, 0x40}});  // swap L/R, half volume
    std::vector<uint8_t> list(8, 0);
    list.insert(list.end(), page.begin(), page.end());
    SendPacketOut(ModeSelectCdb(uint16_t(list.size())), list);
    EXPECT_FALSE(CheckCondition());
    SendPacket({0x5A, 0, 0x0E, 0, 0, 0, 0, 0, 30, 0});
    auto d = DrainData();
    const uint8_t *p = &d[8];
    EXPECT_EQ(p[8] & 0x0F, 0x02);
    EXPECT_EQ(p[9], 0x80);
    EXPECT_EQ(p[10] & 0x0F, 0x01);
    EXPECT_EQ(p[11], 0x40);
}

TEST_F(AtapiCdromTest, ModeSelectRejectsAnythingOtherThanTheAudioControlPage) {
    MountCueBin(MakeCueBin(4, 2));
    SelectDevice0();
    ConsumeResetUnitAttention();
    std::vector<uint8_t> bad_page(16, 0);
    bad_page[0] = 0x0D;  // CD-ROM Parameters page, not Audio Control
    bad_page[1] = 0x0E;
    std::vector<uint8_t> list(8, 0);
    list.insert(list.end(), bad_page.begin(), bad_page.end());
    SendPacketOut(ModeSelectCdb(uint16_t(list.size())), list);
    EXPECT_TRUE(CheckCondition());
    Sense s = RequestSense();
    EXPECT_EQ(s.key, 0x05);
    EXPECT_EQ(s.asc, 0x24) << "INVALID FIELD IN CDB";
}

TEST_F(AtapiCdromTest, MountTruncatesAPartialTrailingSector) {
    // Whole 2048-byte blocks only; a ragged image rounds down.
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
