// WD1003-WA2-compatible hard disk controller: ports 0x1F0-0x1F7 and 0x3F6
// (not 0x3F7, which the floppy owns), IRQ14. Drive C: ships pre-loaded with the
// OS; there is no swap UI. The secondary channel carries the ATAPI CD-ROM
// (atapi_cdrom.h).
//  - The Bochs-legacy BIOS detects the disk as ATA: 0x55/0xAA scratch test,
//    SRST soft reset, signature check, then IDENTIFY DEVICE (0xEC) for geometry.
//    IDENTIFY and 28-bit LBA (Drive/Head bit 6) postdate the 1984 WD1003 and
//    exist only because the firmware assumes them. Machine::configure_factory_cmos()
//    also seeds the CMOS Type 47 geometry, which must match: 1010 cyl / 9 head /
//    55 sec (WD Caviar AC2250, 256MB, see mount()).
//  - READ SECTORS is paced to ~625,000 B/s (ST-506/412 MFM) via one byte-credit
//    target, then done as a bulk copy. WRITE SECTORS commits synchronously
//    when the last byte arrives, as ATA drives can report a write done once
//    data is buffered; rombios.c's ata_cmd_data_io() never waits before checking
//    status. See pio_write_byte().
//  - Multi-sector PIO raises IRQ14 once at the end of the request, not per sector.
#ifndef PC486_WD1003_H
#define PC486_WD1003_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pc486 {

class Wd1003 {
public:
    // WD Caviar AC2250's translated (BIOS setup) geometry -- see mount().
    static constexpr int kCylinders = 1010;
    static constexpr int kHeads = 9;
    static constexpr int kSectorsPerTrack = 55;
    static constexpr std::size_t kImageBytes =
        std::size_t(kCylinders) * kHeads * kSectorsPerTrack * 512;

    struct Drive {
        std::vector<uint8_t> image;
        bool present = false;
        bool dirty = false;
        int cylinders = 0, heads = 0, sectors_per_track = 0;

        static constexpr int kBytesPerSector = 512;
        long offset_for(int cyl, int head, int sector) const {
            return ((long(cyl) * heads + head) * sectors_per_track + (sector - 1)) * kBytesPerSector;
        }
        long capacity_sectors() const { return long(cylinders) * heads * sectors_per_track; }

        // Pages of `image` touched by WRITE SECTORS since clear_dirty(); see dirty_ranges().
        static constexpr std::size_t kDirtyPageSize = 4096;
        std::vector<bool> dirty_page;
    };
    Drive drives[2];  // 0 = C: (this system's only populated drive), 1 = unused (D:, always absent)

    Wd1003() { reset(); }

    void reset();

    bool owns(uint16_t port) const;
    uint8_t in(uint16_t port);
    void out(uint16_t port, uint8_t v);
    // Atomic 16-bit data register path.
    uint16_t data_in16();
    void data_out16(uint16_t v);

    // Inline: called after every instruction.
    void tick(uint64_t cpu_cycles) {
        uint64_t d = cpu_cycles - prev_cycles_;
        prev_cycles_ = cpu_cycles;
        if (xfer_active_) {
            xfer_credit_ += double(d);
            if (xfer_credit_ >= xfer_target_) finish_read_or_write();
        }
    }

    bool irq_pending() const { return irq_pending_; }

    // Pre-load the drive at setup, like a factory-formatted fixed disk.
    void mount(int drive, const uint8_t *data, std::size_t len);
    // Takes ownership of `image` instead of copying it.
    void mount(int drive, std::vector<uint8_t> &&image);

    // True while a command is being serviced or a paced transfer is in flight (activity LED).
    bool busy() const { return (status_ & ST_BSY) != 0 || xfer_active_; }

    // Current image and whether it changed since mount(), so the front end can persist writes.
    const std::vector<uint8_t> &image(int drive) const { return drives[drive & 1].image; }
    bool dirty(int drive) const { return drives[drive & 1].dirty; }
    void clear_dirty(int drive) {
        Drive &d = drives[drive & 1];
        d.dirty = false;
        std::fill(d.dirty_page.begin(), d.dirty_page.end(), false);
    }

    // Byte ranges changed since clear_dirty(), coalesced into runs. Lets
    // persistence store only what was written; copying the whole image stalled
    // the main thread 180-350ms and underran the audio ring. See PC486_REVIEW.md.
    struct DirtyRange { uint32_t offset, length; };
    std::vector<DirtyRange> dirty_ranges(int drive) const {
        std::vector<DirtyRange> out;
        const Drive &d = drives[drive & 1];
        std::size_t i = 0, n = d.dirty_page.size();
        while (i < n) {
            if (!d.dirty_page[i]) { ++i; continue; }
            std::size_t j = i;
            while (j < n && d.dirty_page[j]) ++j;
            uint32_t off = uint32_t(i * Drive::kDirtyPageSize);
            uint32_t end = uint32_t(std::min<std::size_t>(j * Drive::kDirtyPageSize, d.image.size()));
            out.push_back({off, end - off});
            i = j;
        }
        return out;
    }

    // Tracks Machine::cpu_hz() so transfers stay in wall-clock time under Turbo.
    void set_cpu_hz(double hz);

private:
    enum Status : uint8_t {
        ST_ERR = 0x01, ST_DRQ = 0x08, ST_DSC = 0x10, ST_DF = 0x20, ST_DRDY = 0x40, ST_BSY = 0x80,
    };
    enum class PioMode { kNone, kReadDrain, kWriteFill };

    uint8_t error_ = 0;
    uint16_t sector_count_ = 1;   // 0 conventionally means 256 in real ATA
    uint8_t sector_number_ = 1;
    uint8_t cyl_low_ = 0, cyl_high_ = 0;
    uint8_t drive_head_ = 0xA0;   // bits5,7 always 1 on real hardware
    uint8_t status_ = ST_DRDY | ST_DSC;
    // Cylinder Low/High and Status float to 0xFF for the two reads after a soft
    // reset done with the absent slave selected; the BIOS's ata_detect() reads
    // them to conclude there is no second drive. After that they reflect the real
    // device whatever is selected (PC486_REVIEW.md, boot-sector-read bug).
    int floating_reads_left_ = 0;
    bool nien_ = false;           // Device Control bit1: interrupts disabled to the host
    bool srst_prev_ = false;
    int selected_drive_ = 0;
    bool irq_pending_ = false;

    std::vector<uint8_t> pio_buffer_;
    std::size_t pio_pos_ = 0;
    PioMode pio_mode_ = PioMode::kNone;

    // Pending READ/WRITE-SECTORS transfer, paced like fdc765's.
    bool xfer_active_ = false;
    bool xfer_is_write_ = false;
    long xfer_offset_ = 0;
    std::size_t xfer_len_ = 0;
    double xfer_credit_ = 0.0, xfer_target_ = 0.0;
    uint64_t prev_cycles_ = 0;

    // Default = Turbo on (DX2 doubled). set_cpu_hz() updates the live rate.
    static constexpr double kCpuHz = 66000000.0;
    double cpu_hz_ = kCpuHz;
    static constexpr double kBytesPerSec = 625000.0;  // representative ST-506/412-interface MFM rate

    // True when the selected drive has no hardware: drive 1 is permanently absent.
    // Not keyed off Drive::present, which only says whether an image is loaded.
    // Gates only the command register write (0x1F7), so a command aimed at
    // device 1 never executes; ata_detect() needs its IDENTIFY to do nothing.
    // Other task-file writes are shared latches and reads always answer from
    // device 0. The BIOS leaves the slave selected after POST probing and
    // programs CHS/count before reselecting the master, so gating those writes
    // corrupted the first boot-sector read. See PC486_REVIEW.md.
    bool selected_drive_absent() const { return (selected_drive_ & 1) != 0; }

    // Byte offset implied by the task-file registers, CHS or 28-bit LBA.
    long offset_for_current_registers() const;

    void run_command(uint8_t cmd);
    void do_identify();
    void begin_read();
    void begin_write();
    void finish_read_or_write();  // called once the paced transfer completes
    uint8_t pio_read_byte();
    void pio_write_byte(uint8_t v);
};

}  // namespace pc486

#endif  // PC486_WD1003_H
