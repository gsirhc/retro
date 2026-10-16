// NEC uPD765/8272 floppy controller, AT wiring: ports 0x3F0-0x3F7, IRQ6, DMA
// channel 2. Each machine names its drives with a DriveType: the AT's 1.2MB A:
// and 360KB B:, the 486's 3.5" 1.44MB A:. mount() derives geometry from the
// image size, since an HD drive also reads double-density media. Command
// protocol per the Intel 8272A / NEC uPD765A data sheet.
//
// Simplifications (IBM_PCAT_REVIEW.md, PC486_REVIEW.md):
//  - READ/WRITE DATA is paced to the real data rate (500 kbit/s HD, 250 kbit/s
//    DD), then done as one bulk copy. DMA count is not decremented byte by byte
//    mid-transfer.
//  - Seek/recalibrate use a fixed per-track time from the DriveType.
//    SPECIFY is stored but does not affect timing.
//  - FORMAT TRACK takes one revolution and fills the sectors its IDs name,
//    wherever the head is, skipping IDs off the image's geometry.
//  - The data rate written to 0x3F7 is latched but never checked against the
//    media. Pacing uses the media's rate.
//  - The chipset performs the memory<->image copy via transfer_ready(),
//    transfer_image_ptr() and finish_transfer().
#ifndef SHARED_PC_FDC765_H
#define SHARED_PC_FDC765_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pc {

// One drive's mechanism. Media geometry still follows the image size.
struct DriveType {
    int hd_sectors;         // sectors per track on the drive's HD media: 15 (5.25") or 18 (3.5")
    int dd_cylinders;       // 40 for 360KB, 80 for 720KB
    double dd_step_s;       // per-track step time with DD media
    double rpm;             // spindle speed, one FORMAT TRACK revolution
    bool dskchg_needs_disk; // DSKCHG set on eject, cleared only by a STEP with a disk in
};
// 3.5" DSKCHG rule: TEAC FD-235HF spec, "Disk Change".
constexpr DriveType kDrive12M{15, 40, 0.006, 360.0, false};
constexpr DriveType kDrive360K{15, 40, 0.006, 300.0, false};
constexpr DriveType kDrive144M{18, 80, 0.003, 300.0, true};

class Fdc765 {
public:
    struct Drive {
        std::vector<uint8_t> image;
        bool present = false;
        bool write_protected = false;
        int cylinders = 0, heads = 0, sectors_per_track = 0;
        int current_cylinder = 0;
        bool motor_on = false;
        bool dirty = false;
        // DSKCHG is asserted at power-on and after every mount(), and cleared by the
        // next STEP pulse. Swap-aware software polls it to confirm a media change.
        bool disk_changed = true;
        double bytes_per_sec = 0;       // data rate for transfer pacing
        double cycles_per_track_step = 0;  // per-track seek time in CPU cycles

        static constexpr int kBytesPerSector = 512;
        long offset_for(int cyl, int head, int sector) const {
            return ((long(cyl) * heads + head) * sectors_per_track + (sector - 1)) * kBytesPerSector;
        }
        long capacity_bytes() const { return long(cylinders) * heads * sectors_per_track * kBytesPerSector; }
    };
    Drive drives[2];

    Fdc765(double cpu_hz, const DriveType& a, const DriveType& b) : cpu_hz_(cpu_hz), types_{a, b} { reset(); }

    void reset();

    // 0x3F6 is not ours: it is the HDD Device Control / Alternate Status register
    // (wd1003.h).
    bool owns(uint16_t port) const { return (port >= 0x3F0 && port <= 0x3F5) || port == 0x3F7; }
    uint8_t in(uint16_t port);
    void out(uint16_t port, uint8_t v);

    // Advances seek and transfer pacing against the CPU cycle count. Inline: the
    // 486 calls it after every instruction (PC486_REVIEW.md §8).
    void tick(uint64_t cpu_cycles) {
        uint64_t d = cpu_cycles - prev_cycles_;
        prev_cycles_ = cpu_cycles;
        if (seeking_) {
            seek_credit_ += double(d);
            if (seek_credit_ >= seek_target_) {
                seeking_ = false;
                seek_pending_irq_ = true;
                irq_pending_ = true;
            }
        }
        if (xfer_active_) {
            xfer_credit_ += double(d);
            if (xfer_credit_ >= xfer_target_) transfer_ready_ = true;
        }
    }

    // DOR bit 3 gates the INT and DRQ drivers, not the FDC's INT (WD1003-WA2 OEM manual, FDMAEN).
    bool irq_pending() const { return irq_pending_ && dma_enabled(); }
    void clear_irq() { irq_pending_ = false; }

    // 0 = 500, 1 = 300, 2 = 250 kbps MFM, 3 = 125 kbps FM.
    uint8_t data_rate() const { return data_rate_; }

    // --- chipset/DMA integration ---
    bool transfer_ready() const { return transfer_ready_; }
    bool dma_enabled() const { return (dor_ & 0x08) != 0; }
    bool transfer_is_write() const { return transfer_is_write_; }  // memory -> image
    std::size_t transfer_length() const { return transfer_len_; }
    uint8_t *transfer_image_ptr();
    void finish_transfer(std::size_t actual_len);

    // --- host/front-end side ---
    void mount(int drive, const uint8_t *data, std::size_t len);
    void unmount(int drive);
    bool mounted(int drive) const { return drives[drive & 1].present; }
    bool dirty(int drive) const { return drives[drive & 1].dirty; }
    void clear_dirty(int drive) { drives[drive & 1].dirty = false; }

    // Keeps seek and transfer times in wall-clock time when the CPU clock changes (486 Turbo).
    void set_cpu_hz(double hz);

private:
    enum class Phase { kIdle, kCommand, kExecution, kResult };
    Phase phase_ = Phase::kIdle;

    uint8_t dor_ = 0x00;        // Digital Output Register (0x3F2)
    uint8_t data_rate_ = 0;     // Floppy Control register (0x3F7 write)

    uint8_t cmd_ = 0;
    uint8_t params_[9] = {};
    int param_count_needed_ = 0, params_received_ = 0;
    uint8_t result_[7] = {};
    int result_count_ = 0, result_sent_ = 0;

    // Pending seek/recalibrate interrupt, reported by SENSE INTERRUPT STATUS (0x08).
    bool seek_pending_irq_ = false;
    int seek_drive_ = 0;
    double seek_credit_ = 0.0, seek_target_ = 0.0;
    bool seeking_ = false;

    bool irq_pending_ = false;

    // Active data-transfer command state
    bool transfer_ready_ = false;
    bool transfer_is_write_ = false;
    int xfer_drive_ = 0;
    long xfer_offset_ = 0;
    std::size_t transfer_len_ = 0;
    double xfer_credit_ = 0.0, xfer_target_ = 0.0;
    bool xfer_active_ = false;
    bool formatting_ = false;
    uint8_t format_fill_ = 0;
    std::vector<uint8_t> format_ids_;  // C, H, R, N per sector, read by DMA

    uint64_t prev_cycles_ = 0;

    double cpu_hz_;
    DriveType types_[2];

    uint8_t msr() const;
    void start_command(uint8_t first_byte);
    void run_command();  // called once all parameter bytes are in
    void begin_transfer(bool is_write);
    void begin_format();
    void apply_format(std::size_t id_bytes);
};

}  // namespace pc

#endif  // SHARED_PC_FDC765_H
