// NEC uPD765/8272 floppy controller, AT wiring: ports 0x3F0-0x3F7, IRQ6, DMA
// channel 2. Drive A: 1.2MB (80/2/15, 360 RPM), B: 360KB (40/2/9, 300 RPM).
// mount() derives geometry from the image size, since a 1.2MB drive also reads
// 360KB media. Command protocol per the Intel 8272A / NEC uPD765A data sheet.
//
// Simplifications (IBM_PCAT_REVIEW.md):
//  - READ/WRITE DATA is paced to the real data rate (500 kbit/s for 1.2MB,
//    250 kbit/s for 360KB), then done as one bulk copy. DMA count is not
//    decremented byte by byte mid-transfer.
//  - Seek/recalibrate use a fixed per-track time (3ms 1.2MB, 6ms 360KB).
//    SPECIFY is stored but does not affect timing.
//  - FORMAT TRACK returns the right byte counts but does not reformat.
//  - chipset.cpp performs the memory<->image copy via transfer_ready(),
//    transfer_image_ptr() and finish_transfer().
#ifndef IBMPCAT_FDC765_H
#define IBMPCAT_FDC765_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ibmpcat {

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
    Drive drives[2];  // 0 = A: (1.2MB), 1 = B: (360KB)

    Fdc765() { reset(); }

    void reset();

    // 0x3F6 is not ours: it is the HDD Device Control / Alternate Status register
    // (wd1003.h).
    bool owns(uint16_t port) const { return (port >= 0x3F0 && port <= 0x3F5) || port == 0x3F7; }
    uint8_t in(uint16_t port);
    void out(uint16_t port, uint8_t v);

    // Advances seek and transfer pacing against the CPU cycle count.
    void tick(uint64_t cpu_cycles);

    bool irq_pending() const { return irq_pending_; }
    void clear_irq() { irq_pending_ = false; }

    // --- chipset/DMA integration ---
    bool transfer_ready() const { return transfer_ready_; }
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

private:
    enum class Phase { kIdle, kCommand, kExecution, kResult };
    Phase phase_ = Phase::kIdle;

    uint8_t dor_ = 0x00;        // Digital Output Register (0x3F2)

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

    uint64_t prev_cycles_ = 0;

    static constexpr double kCpuHz = 8000000.0;  // this machine's fixed real clock

    uint8_t msr() const;
    void start_command(uint8_t first_byte);
    void run_command();  // called once all parameter bytes are in
    void begin_transfer(bool is_write);
};

}  // namespace ibmpcat

#endif  // IBMPCAT_FDC765_H
