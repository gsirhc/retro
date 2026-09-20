// NEC uPD765/8272 floppy disk controller, ISA wiring: ports 0x3F0-0x3F7,
// IRQ6, DMA channel 2 -- unchanged from the AT (a period 486 board's Super
// I/O chip integrates the same uPD765-compatible core rather than a
// discrete part, but the register protocol is identical). One drive -- A:
// a 3.5" 1.44MB high-density floppy (80 cyl/2 head/18 sec/track, 300 RPM,
// 500 kbit/s), the single bay this "period-correct 3.5 inch floppy" build
// specifies; no B: connector exists on this machine. mount() derives the
// live cylinder/head/sector geometry from the mounted image's own size
// rather than hardcoding it, since a real 1.44MB drive can also read/write
// genuine double-density 720KB 3.5" media (a real, period-legal
// combination, just at a different data rate, not a different drive) --
// see mount()'s own comment. Command protocol (command byte -> parameter
// bytes -> execution -> result bytes) per the Intel 8272A / NEC uPD765A
// data sheet.
//
// Scope/simplifications (see PC486_REVIEW.md):
//  - A data-transfer command (READ/WRITE DATA) is paced to the real drive
//    data rate (500 kbit/s for 1.44MB media, 250 kbit/s for 720KB media --
//    the same real distinction disk88.h's RPM constants preserve) via a
//    single accumulated byte-credit target, then executed as one bulk
//    memory<->image copy once that much real time has elapsed -- real
//    total transfer TIME is respected (never sped up, per CLAUDE.md), but
//    the DMA address/count registers are NOT visibly decremented one byte
//    at a time mid-transfer the way real hardware's cycle-stealing bus
//    arbitration would show a bus analyzer. Software that waits for the
//    completion IRQ (universal practice) can't tell the difference;
//    software that polls DMA count mid-transfer (very rare) could.
//  - Seek/recalibrate step timing uses a fixed 3ms/track constant (a
//    representative period 3.5" HD drive figure) rather than interpreting
//    SPECIFY's programmed step-rate register -- accepted and stored for
//    read-back, but not used to compute timing.
//  - FORMAT TRACK is accepted (correct parameter/result byte counts, so a
//    driver probing for it doesn't get confused) but does not actually
//    reformat -- this machine's images always arrive pre-formatted.
//  - `chipset.cpp` orchestrates the actual memory<->image byte copy via
//    transfer_ready()/transfer_image_ptr()/finish_transfer(), the same way
//    it orchestrates the PIC master/slave cascade -- this device never
//    reaches into system memory or the DMA controller directly.
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
        // Real hardware: a drive's DSKCHG line is asserted at power-on and
        // whenever the door has been opened (media possibly swapped), and
        // is only cleared once a STEP pulse (RECALIBRATE/SEEK) is actually
        // issued to that drive afterward -- software that swaps floppies
        // mid-session (e.g. a multi-disk installer's file copier) polls
        // this specifically to confirm the user really did swap media
        // before trusting anything it reads next. Starts true (matching
        // real power-on) and true again after every mount().
        bool disk_changed = true;
        double bytes_per_sec = 0;       // real data rate driving transfer pacing
        double cycles_per_track_step = 0;  // real per-track seek time, in CPU cycles

        static constexpr int kBytesPerSector = 512;
        long offset_for(int cyl, int head, int sector) const {
            return ((long(cyl) * heads + head) * sectors_per_track + (sector - 1)) * kBytesPerSector;
        }
        long capacity_bytes() const { return long(cylinders) * heads * sectors_per_track * kBytesPerSector; }
    };
    Drive drives[2];  // 0 = A: (3.5" 1.44MB, this system's only floppy bay);
                       // 1 is permanently unpopulated -- no B: connector on
                       // this build. Kept as a 2-element array rather than
                       // resized, matching wd1003.h's "permanently absent
                       // second device" convention for the same reason: the
                       // controller's own drive-select bits (DOR bits 0-1,
                       // motor bits 4-5) are a 2-drive protocol regardless
                       // of what's actually cabled.

    Fdc765() { reset(); }

    void reset();

    // 0x3F0-0x3F5 and 0x3F7 only -- 0x3F6, in the middle of this otherwise
    // contiguous-looking range, is genuinely NOT decoded by the floppy
    // controller on a real AT. It belongs to the hard disk controller's
    // Device Control / Alternate Status register instead (see wd1003.h);
    // claiming it here would silently steal it from the HDD before
    // Chipset::io_in/io_out ever got to check hdd.owns(). A real bug this
    // session's WD1003 integration testing caught. See PC486_REVIEW.md.
    bool owns(uint16_t port) const { return (port >= 0x3F0 && port <= 0x3F5) || port == 0x3F7; }
    uint8_t in(uint16_t port);
    void out(uint16_t port, uint8_t v);

    // Advances seek and transfer pacing against the CPU's running cycle
    // count (absolute, like CassetteACR::tick). Defined inline because
    // Machine::run_cycles() calls it after every instruction and an idle
    // drive -- the overwhelmingly common case -- is then just two loads and
    // two not-taken branches. See PC486_REVIEW.md §8.
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

    // --- chipset/DMA integration (see file header) -----------------------
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

private:
    enum class Phase { kIdle, kCommand, kExecution, kResult };
    Phase phase_ = Phase::kIdle;

    uint8_t dor_ = 0x00;        // Digital Output Register (0x3F2)

    uint8_t cmd_ = 0;
    uint8_t params_[9] = {};
    int param_count_needed_ = 0, params_received_ = 0;
    uint8_t result_[7] = {};
    int result_count_ = 0, result_sent_ = 0;

    // Pending seek/recalibrate (implied-seek) interrupt state, surfaced via
    // SENSE INTERRUPT STATUS (0x08).
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

    // This machine's real 66 MHz clock (see machine.h's kCpuHz) -- Chipset
    // feeds tick() a cycle counter paced at that rate, so this pacing
    // constant must match it or transfers run at the wrong wall-clock speed.
    static constexpr double kCpuHz = 66000000.0;

    uint8_t msr() const;
    void start_command(uint8_t first_byte);
    void run_command();  // called once all parameter bytes are in
    void begin_transfer(bool is_write);
};

}  // namespace pc486

#endif  // PC486_FDC765_H
