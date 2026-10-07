// NEC uPD765/8272 floppy controller: ports 0x3F0-0x3F7, IRQ6, DMA channel 2.
// One drive, A: a 3.5" 1.44MB (80/2/18, 300 RPM, 500 kbit/s). mount() derives
// geometry from the image size so 720KB media also works. Command protocol per
// the Intel 8272A / NEC uPD765A data sheet.
//  - READ/WRITE DATA is paced to the real data rate (500 kbit/s, 250 for 720KB)
//    via one byte-credit target, then done as a single bulk copy. DMA
//    address/count do not decrement mid-transfer.
//  - Seek/recalibrate use a fixed 3ms/track; SPECIFY is stored but not used.
//  - FORMAT TRACK has the right parameter/result counts but does not reformat.
//  - chipset.cpp does the memory<->image copy via transfer_ready() /
//    transfer_image_ptr() / finish_transfer().
#ifndef PC486_FDC765_H
#define PC486_FDC765_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pc486 {

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
        // DSKCHG: set at power-on and when the door opens, cleared only by a STEP
        // pulse. Swap-aware software polls it. True after every mount().
        bool disk_changed = true;
        double bytes_per_sec = 0;       // real data rate driving transfer pacing
        double cycles_per_track_step = 0;  // real per-track seek time, in CPU cycles

        static constexpr int kBytesPerSector = 512;
        long offset_for(int cyl, int head, int sector) const {
            return ((long(cyl) * heads + head) * sectors_per_track + (sector - 1)) * kBytesPerSector;
        }
        long capacity_bytes() const { return long(cylinders) * heads * sectors_per_track * kBytesPerSector; }
    };
    Drive drives[2];  // 0 = A:; 1 is permanently unpopulated (DOR select bits are a 2-drive protocol)

    Fdc765() { reset(); }

    void reset();

    // 0x3F6 is not decoded here; it is the HDD's Device Control / Alternate Status (wd1003.h).
    bool owns(uint16_t port) const { return (port >= 0x3F0 && port <= 0x3F5) || port == 0x3F7; }
    uint8_t in(uint16_t port);
    void out(uint16_t port, uint8_t v);

    // Inline: called after every instruction. See PC486_REVIEW.md §8.
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

    bool irq_pending() const { return irq_pending_; }
    void clear_irq() { irq_pending_ = false; }

    // --- chipset/DMA integration ---
    bool transfer_ready() const { return transfer_ready_; }
    bool transfer_is_write() const { return transfer_is_write_; }  // memory -> image
    std::size_t transfer_length() const { return transfer_len_; }
    uint8_t *transfer_image_ptr();
    void finish_transfer(std::size_t actual_len);

    // --- host/front-end side, matching disk88.h's mount convention -------
    void mount(int drive, const uint8_t *data, std::size_t len);
    void unmount(int drive);
    bool mounted(int drive) const { return drives[drive & 1].present; }
    bool dirty(int drive) const { return drives[drive & 1].dirty; }
    void clear_dirty(int drive) { drives[drive & 1].dirty = false; }

    // Tracks Machine::cpu_hz() so transfers stay in wall-clock time under Turbo.
    void set_cpu_hz(double hz);

private:
    enum class Phase { kIdle, kCommand, kExecution, kResult };
    Phase phase_ = Phase::kIdle;

    uint8_t dor_ = 0x00;        // Digital Output Register (0x3F2)

    uint8_t cmd_ = 0;
    uint8_t params_[9] = {};
    int param_count_needed_ = 0, params_received_ = 0;
    uint8_t result_[7] = {};
    int result_count_ = 0, result_sent_ = 0;

    // Pending seek/recalibrate interrupt state, read via SENSE INTERRUPT STATUS.
    bool seek_pending_irq_ = false;
    int seek_drive_ = 0;
    double seek_credit_ = 0.0, seek_target_ = 0.0;
    bool seeking_ = false;

    bool irq_pending_ = false;

    // Active data-transfer command state.
    bool transfer_ready_ = false;
    bool transfer_is_write_ = false;
    int xfer_drive_ = 0;
    long xfer_offset_ = 0;
    std::size_t transfer_len_ = 0;
    double xfer_credit_ = 0.0, xfer_target_ = 0.0;
    bool xfer_active_ = false;

    uint64_t prev_cycles_ = 0;

    // Updated by set_cpu_hz().
    static constexpr double kCpuHz = 66000000.0;
    double cpu_hz_ = kCpuHz;

    uint8_t msr() const;
    void start_command(uint8_t first_byte);
    void run_command();  // called once all parameter bytes are in
    void begin_transfer(bool is_write);
};

}  // namespace pc486

#endif  // PC486_FDC765_H
