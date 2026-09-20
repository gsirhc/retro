// ATAPI CD-ROM drive on this machine's SECONDARY IDE channel: ports
// 0x170-0x177 (command block) + 0x376 (control block, Device Control /
// Alternate Status), IRQ15 -- the standard secondary-channel convention,
// the exact structural mirror of the primary channel's 0x1F0-0x1F7/0x3F6/
// IRQ14 that wd1003.h implements. A real 1993-94 gaming PC cabled the hard
// disk and the CD-ROM to separate channels precisely because a 2x CD-ROM's
// ~250 ms access time would otherwise hold the shared cable and stall every
// disk request behind it.
//
// Modeled as the sole device on that channel (device 0/master, 2x speed,
// tray loader). Device 1 is permanently unpopulated -- see
// selected_device_absent(), which implements the real "Device 0 responding
// for Device 1" rules rather than wd1003's floated-register model, because
// the spec genuinely says something DIFFERENT for a packet device than for
// a disk. The present master's post-reset signature here is likewise the
// ATAPI signature (0x14/0xEB), not an ATA disk's 0x00/0x00.
//
// Protocol per ATA/ATAPI-4 (NCITS 317-1998, X3T13/1153D rev 17) and the
// SCSI-3 MMC command set (NCITS 304 / SFF-8020i, "ATA Packet Interface for
// CD-ROMs"), with SPC (NCITS 301) for the commands MMC inherits (TEST UNIT
// READY, REQUEST SENSE, INQUIRY, MODE SENSE, START STOP UNIT, PREVENT
// ALLOW MEDIUM REMOVAL). Specific citations live at the behavior they
// justify; the round-up is in PC486_REVIEW.md.
//
// Real ATA/ATAPI behaviors preserved here on purpose, each load-bearing for
// device detection or for a DOS-side ATAPI/MSCDEX driver:
//  - The post-reset ATAPI signature (Cylinder Low = 0x14, Cylinder High =
//    0xEB, Sector Count/Number = 1, Status = 0x00) -- ATA/ATAPI-4 §9.1
//    "Signature and persistence". This is how a BIOS/driver tells "ATA
//    disk" from "ATAPI packet device" on an otherwise identical register
//    block, so it is the single most load-bearing fact in this file.
//  - Status bit 6 (DRDY) is CLEAR after a reset and is only set once a
//    command has actually completed. An ATAPI device genuinely does not
//    report itself "ready" the way a disk does (ATA/ATAPI-4 §9.1's
//    signature table lists Status = 00h); software that waits for DRDY on a
//    packet device hangs forever, which is exactly why ATAPI drivers poll
//    BSY instead.
//  - Status bit 4 is SERVICE and bit 5 is DMA-READY on a packet device --
//    NOT the ATA disk's DSC (seek complete). wd1003 sets 0x10 on every
//    completion; this device deliberately never does, because here that bit
//    would be a false "service request pending" for overlapped commands
//    (ATA/ATAPI-4 §7.15.6.3).
//  - The Error register on a packet device carries the SCSI sense key in
//    bits 7:4 (plus MCR/ABRT/EOM/ILI in the low nibble), not an ATA disk's
//    IDNF/UNC/etc. bit field -- ATA/ATAPI-4 §7.6.1.
//  - The register at offset 2 is one physical latch serving double duty:
//    the host writes it as Sector Count (unused by ATAPI) and reads it as
//    the Interrupt Reason register, and the device only overwrites it at a
//    phase transition. Offsets 4/5 behave the same way (host writes the
//    byte-count LIMIT, device overwrites with the actual block byte count).
//    This is not a convenience -- the legacy-BIOS detection path writes
//    0x55/0xAA to offsets 2/3 and requires them to read back before it will
//    even attempt a reset/signature probe, and it works on real drives
//    exactly because these are shared latches.
//  - "Device 0 responding for Device 1" answers a probe of the absent
//    device 1 with 00h rather than this device's own registers, because
//    ATA/ATAPI-6 Table 18 spells out a different response for a PACKET
//    device than for a disk -- see selected_device_absent().
//  - IDENTIFY DEVICE (0xEC) is ABORTED, with the ATAPI signature placed in
//    the task file, rather than answered -- ATA/ATAPI-4 §8.12.1. That's the
//    second half of the same detection mechanism: a host that issues the
//    ATA identify anyway still learns it's talking to a packet device.
//  - A unit-attention condition is raised on reset (ASC 29h "power on,
//    reset, or bus device reset occurred") and on every media change (ASC
//    28h "not ready to ready change, medium may have changed"), reported as
//    CHECK CONDITION on the next command other than INQUIRY or REQUEST
//    SENSE, then cleared -- SPC §5.6. This is the mechanism a DOS CD-ROM
//    driver's "has the disc changed?" poll actually rides on.
//
// Scope/simplifications (see PC486_REVIEW.md):
//  - PIO only. No DMA: the PACKET command's Features-register DMA bit is
//    rejected with ABRT, and IDENTIFY PACKET DEVICE advertises no DMA
//    modes, so a driver negotiates PIO instead of programming a bus-master
//    controller this machine doesn't have.
//  - Data-in commands are paced to a real 2x drive's timing (307,200
//    bytes/sec sustained, ~250 ms average access on a non-sequential
//    request) via one accumulated cycle-credit target, then the whole
//    result is handed over as the DRQ blocks drain -- the same "respect the
//    real total transfer TIME, not the byte-by-byte bus handshake" tradeoff
//    wd1003.h and fdc765.h already make, for the same reason.
//  - Data-OUT packet commands (MODE SELECT, WRITE) are not implemented:
//    this is a read-only CD-ROM, and none of the implemented CDBs transfer
//    data to the device. An unimplemented opcode gets a genuine CHECK
//    CONDITION / ILLEGAL REQUEST / INVALID COMMAND OPERATION CODE rather
//    than silence.
//  - Audio playback CDBs (PLAY AUDIO, READ CD-DA) are not implemented, and
//    the MODE SENSE capabilities page honestly reports no audio support
//    rather than advertising a feature that would then fail.
#ifndef PC486_ATAPI_CDROM_H
#define PC486_ATAPI_CDROM_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pc486 {

class AtapiCdrom {
public:
    // CD-ROM Mode 1 user data per sector. Not 512: an ATAPI CD-ROM's block
    // size is genuinely 2048 bytes, which is why READ CAPACITY has to report
    // it and why a DOS driver has to block-translate for INT 2Fh.
    static constexpr int kBytesPerSector = 2048;

    AtapiCdrom() { reset(); }

    void reset();

    bool owns(uint16_t port) const;
    uint8_t in(uint16_t port);
    void out(uint16_t port, uint8_t v);
    // The data register (0x170) needs a genuine atomic 16-bit path, same as
    // the primary channel's 0x1F0 -- see cpu80286.h's Bus::in16/out16
    // comment. Every ATAPI transfer, the 12-byte command packet included, is
    // a sequence of 16-bit words.
    uint16_t data_in16();
    void data_out16(uint16_t v);

    // Inline for the same reason as Fdc765::tick -- called after every
    // instruction, and an idle drive costs one load and one branch.
    void tick(uint64_t cpu_cycles) {
        uint64_t delta = cpu_cycles - prev_cycles_;
        prev_cycles_ = cpu_cycles;
        if (exec_active_) {
            exec_credit_ += double(delta);
            if (exec_credit_ >= exec_target_) finish_execute();
        }
    }

    bool irq_pending() const { return irq_pending_; }

    // Removable media, like fdc765's drives and unlike wd1003's fixed disk:
    // `data` is a plain 2048-byte-sector ISO 9660 image (MODE1/2048, the
    // form every DOS-era data CD ships as).
    void mount(const uint8_t *data, std::size_t len);
    void eject();
    bool media_present() const { return media_present_; }

    // Host/front-end convenience: true while a command is actually being
    // serviced -- what a real drive's activity LED lights for.
    bool busy() const { return (status_ & ST_BSY) != 0 || exec_active_; }

private:
    // Status register, PACKET device flavor (ATA/ATAPI-4 §7.15.6.3). Note
    // what is NOT here: bit 0 is CHK (check condition), not ATA's ERR, and
    // bits 4/5 are SERVICE/DMA-READY, not ATA's DSC/DF.
    enum Status : uint8_t {
        ST_CHK = 0x01, ST_DRQ = 0x08, ST_SERV = 0x10, ST_DMRD = 0x20,
        ST_DRDY = 0x40, ST_BSY = 0x80,
    };
    // Interrupt Reason register (read side of offset 2), ATA/ATAPI-4 §7.12.
    enum IntReason : uint8_t {
        IR_CD = 0x01,   // 1 = a command packet is being transferred
        IR_IO = 0x02,   // 1 = transfer is to the host, 0 = to the device
        IR_REL = 0x04,  // bus release (overlap mode; never set here)
    };
    enum class Phase {
        kIdle,
        kCommandPacket,  // DRQ up, awaiting the 12-byte CDB via the data register
        kExecuting,      // BSY up, paced to real drive timing
        kDataIn,         // DRQ up, host draining one data block
    };

    // --- task file ------------------------------------------------------
    uint8_t error_ = 0x01;        // post-reset diagnostic code; sense key in 7:4 after a failure
    uint8_t features_ = 0;
    // One physical latch each, written as Sector Count / read as Interrupt
    // Reason, and written as the byte-count limit / read as the current
    // block's byte count -- see the file header.
    uint8_t int_reason_ = 0x01;
    uint8_t sector_number_ = 0x01;
    uint16_t byte_count_ = 0xEB14;  // low = 0x14, high = 0xEB: the ATAPI signature
    uint8_t drive_head_ = 0xA0;     // bits 5,7 always 1, same legacy fact as wd1003's
    uint8_t status_ = 0x00;         // DRDY deliberately clear after reset -- see header

    bool nien_ = false;   // Device Control bit 1: interrupts masked to the host
    bool srst_prev_ = false;
    int selected_device_ = 0;
    bool irq_pending_ = false;

    // Device 1 on this channel is permanently unpopulated, so device 0 is
    // "responding for device 1" -- a genuine, fully specified mode, not a
    // guess: ATA/ATAPI-6 (T13/1410D revision 3a) Table 18, "Device 1 is
    // selected and Device 0 is responding for Device 1", which folded in
    // proposal E00118R0's ATAPI half. That table is explicit that a device
    // implementing the PACKET command set behaves DIFFERENTLY here than a
    // disk does, which is why this device deliberately does NOT copy
    // wd1003's floated-cylinder-register model:
    //   - Reads of Sector Count, LBA Low/Mid/High and the Device register
    //     "place 00h on the data bus" for a PACKET device, where a non-
    //     packet device places device 0's own register contents (exactly
    //     what wd1003 does -- both are right, for their own device type).
    //   - Reads of Status and Alternate Status place 00h for either.
    //   - The Error register reads device 0's real Error register.
    //   - Every register WRITE lands in device 0's register, command
    //     register included, but "do not respond unless the command is
    //     EXECUTE DEVICE DIAGNOSTICS".
    // This is load-bearing, not pedantry: FreeDOS's real ATAPICDD.SYS
    // probes device 1 by writing 0x55/0xAA to Sector Count/Number and
    // reading them back, then (post-reset) by checking Sector
    // Count/Number == 01h/01h before testing for the 14h/EBh signature.
    // Handing back the shared latch contents -- wd1003's correct behavior
    // for a disk -- would pass both of those probes and invent a phantom
    // ATAPI slave for the driver to time out against.
    bool selected_device_absent() const { return (selected_device_ & 1) != 0; }

    // --- packet / data state --------------------------------------------
    Phase phase_ = Phase::kIdle;
    uint8_t cdb_[12] = {};
    int cdb_pos_ = 0;
    uint16_t limit_ = 0;   // host's per-DRQ-block byte count limit, captured at PACKET

    std::vector<uint8_t> data_;
    std::size_t data_pos_ = 0;
    std::size_t block_end_ = 0;  // end of the DRQ block currently being drained

    // --- sense data (SPC fixed-format) ----------------------------------
    uint8_t sense_key_ = 0, asc_ = 0, ascq_ = 0;
    bool check_pending_ = false;
    bool ua_pending_ = false;
    uint8_t ua_asc_ = 0, ua_ascq_ = 0;
    bool locked_ = false;  // PREVENT ALLOW MEDIUM REMOVAL

    // --- media ----------------------------------------------------------
    std::vector<uint8_t> image_;
    bool media_present_ = false;

    // --- pacing ---------------------------------------------------------
    bool exec_active_ = false;
    double exec_credit_ = 0.0, exec_target_ = 0.0;
    uint64_t prev_cycles_ = 0;
    // Where the head is, which is also where the read-ahead buffer ends.
    // kHeadUnknown means "cold": just reset or the media just changed, so
    // nothing is buffered and the head is nowhere in particular.
    static constexpr uint32_t kHeadUnknown = 0xFFFFFFFF;
    uint32_t head_lba_ = kHeadUnknown;

    // The cycle count Chipset feeds tick() is this machine's 80486DX2-66
    // internal clock. If that counter's units ever change (e.g. to 33 MHz
    // bus clocks), this constant changes with it or every paced delay is
    // wrong by the same factor.
    static constexpr double kCpuHz = 66000000.0;
    // 2x CD-ROM: 1x is 75 sectors/sec of 2048-byte user data = 153,600
    // bytes/sec, so 2x is exactly double. Not a round "300 KB/s".
    static constexpr double kBytesPerSec = 307200.0;
    // Average access time of a period 2x drive (Mitsumi/Sony/Matsushita
    // datasheets of the era quote 250-350 ms, an order of magnitude worse
    // than the hard disk). "Average" in those datasheets means a one-third-
    // stroke seek, which is what access_seconds_for() calibrates against --
    // it is NOT the cost of every seek regardless of distance.
    static constexpr double kAccessSec = 0.25;
    // Short-seek floor: even an adjacent-track move costs real time on a CD
    // (sled step plus the CLV spindle-speed change), and period 2x drives
    // quote 80-150 ms for a single-track/short access. The conservative low
    // end of that range.
    static constexpr double kSeekMinSec = 0.08;
    // Read-ahead buffer, in 2048-byte sectors: 32 = 64KB. Period 2x drives
    // shipped 64KB-256KB buffers; this is deliberately the low (slowest) end
    // of that range. A request starting inside the buffered region needs no
    // head movement at all, which is the case that dominates real DOS use of
    // a CD -- a batch file, the utilities it invokes and the ISO directory
    // extents get re-read constantly, all within a few tens of KB.
    static constexpr uint32_t kBufferSectors = 32;
    // Fixed per-command overhead: a real drive takes a few ms to decode and
    // answer even a no-data command like TEST UNIT READY.
    static constexpr double kCommandSec = 0.001;

    // Head-position / read-ahead-buffer model behind the transfer pacing.
    double access_seconds_for(uint32_t lba) const;
    void note_transfer(uint32_t lba, uint32_t blocks);

    void assert_signature();
    void run_command(uint8_t cmd);
    void identify_packet_device();
    void device_reset();
    void abort_command(bool with_signature);

    void begin_packet();
    void execute_packet();
    void begin_execute(double seconds);
    void finish_execute();
    void start_data_in_block();
    void command_complete();

    uint8_t read_data_byte();
    void write_data_byte(uint8_t v);

    // Sense/status helpers. set_check() also loads the Error register's
    // sense-key nibble (ATA/ATAPI-4 §7.6.1).
    void set_check(uint8_t key, uint8_t asc, uint8_t ascq, bool abrt = false);
    void set_unit_attention(uint8_t asc, uint8_t ascq);
    bool require_media();
    void respond(const uint8_t *bytes, std::size_t len, std::size_t alloc);

    uint32_t capacity_blocks() const;

    // CDB handlers. cmd_read10() returns the extra real time its transfer
    // costs, on top of kCommandSec.
    void cmd_request_sense();
    void cmd_inquiry();
    void cmd_start_stop_unit();
    void cmd_read_capacity();
    double cmd_read10();
    double cmd_seek10();
    void cmd_read_toc();
    void cmd_mode_sense10();
};

}  // namespace pc486

#endif  // PC486_ATAPI_CDROM_H
