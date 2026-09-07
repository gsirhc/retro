#include "fdc765.h"

#include <algorithm>
#include <cstdlib>

namespace ibmpcat {

void Fdc765::reset() {
    phase_ = Phase::kIdle;
    dor_ = 0;
    cmd_ = 0;
    params_received_ = param_count_needed_ = 0;
    result_count_ = result_sent_ = 0;
    seek_pending_irq_ = false;
    seeking_ = false;
    seek_credit_ = seek_target_ = 0.0;
    irq_pending_ = false;
    transfer_ready_ = false;
    xfer_active_ = false;
    prev_cycles_ = 0;
    // Mounted media survives a controller reset, matching real hardware --
    // only the controller's own transient state is cleared here.
    for (auto &d : drives) {
        d.current_cylinder = 0;
        d.motor_on = false;
        d.disk_changed = true;  // real DSKCHG: asserted at power-on/reset
    }
}

void Fdc765::mount(int drive, const uint8_t *data, std::size_t len) {
    Drive &d = drives[drive & 1];
    d.image.assign(data, data + len);
    d.present = true;
    d.dirty = false;
    d.write_protected = false;
    d.current_cylinder = 0;
    d.disk_changed = true;  // real DSKCHG: asserted whenever media is swapped
    // Geometry follows the actual media, not just which bay it's in -- a
    // real 5.25" high-density drive (this system's A:) mechanically and
    // magnetically CAN read/write a genuine double-density 360KB diskette
    // (a different data rate/step timing, not a different drive), so a
    // 360KB image dropped into A: is a real, period-legal combination, not
    // an error. A 360KB-only drive (B:) can't go the other way -- it
    // physically cannot read high-density media at all (different magnetic
    // coercivity) -- but this emulator doesn't enforce that rejection (see
    // the file header's scope note: images arrive pre-formatted, no
    // physical media-compatibility checks). Previously this branched on
    // `drive` alone, so a 360KB image mounted in A: kept the drive's own
    // 80/2/15 geometry regardless -- CHS math beyond the very first sector
    // (offset 0 under any geometry) landed on the wrong bytes or ran past
    // the image entirely, which chipset.cpp's DMA path silently treats as
    // "transfer completed, zero bytes moved" rather than a real disk
    // error -- IO.SYS's own loader would appear to succeed and then jump
    // into garbage, hanging exactly where a real boot would instead get a
    // real controller error it could act on.
    if (len <= 368640) {
        // 360KB: 40 cyl / 2 head / 9 sec/track, 250 kbit/s, ~6ms/track step.
        d.cylinders = 40; d.heads = 2; d.sectors_per_track = 9;
        d.bytes_per_sec = 31250.0;
        d.cycles_per_track_step = 0.006 * kCpuHz;
    } else {
        // 1.2MB: 80 cyl / 2 head / 15 sec/track, 500 kbit/s, ~3ms/track step.
        d.cylinders = 80; d.heads = 2; d.sectors_per_track = 15;
        d.bytes_per_sec = 62500.0;
        d.cycles_per_track_step = 0.003 * kCpuHz;
    }
}
void Fdc765::unmount(int drive) {
    Drive &d = drives[drive & 1];
    d.image.clear();
    d.present = false;
}

uint8_t Fdc765::msr() const {
    uint8_t v = 0;
    if (phase_ != Phase::kExecution) v = uint8_t(v | 0x80);  // RQM: ready for the next byte
    if (phase_ == Phase::kResult) v = uint8_t(v | 0x40);     // DIO: FDC has data for the CPU
    if (phase_ != Phase::kIdle) v = uint8_t(v | 0x10);       // FDC busy
    if (seeking_) v = uint8_t(v | (1 << (seek_drive_ & 1)));
    return v;
}

uint8_t Fdc765::in(uint16_t port) {
    switch (port) {
        case 0x3F2: return dor_;
        case 0x3F4: return msr();
        case 0x3F5:
            if (phase_ == Phase::kResult) {
                uint8_t v = result_[result_sent_++];
                // Real uPD765/8272 hardware drops the INT line as soon as
                // the CPU reads the *first* result byte (ST0) -- not after
                // the whole result phase is drained. Getting this wrong is
                // a real bug this session hit: some real driver code reads
                // only ST0 (or a handful of the 7 result bytes) before
                // moving on, and modeling "IRQ clears on full drain"
                // instead left the interrupt permanently pending,
                // re-triggering the ISR every single tick forever. See
                // IBM_PCAT_REVIEW.md §8.
                if (result_sent_ == 1) irq_pending_ = false;
                if (result_sent_ >= result_count_) phase_ = Phase::kIdle;
                return v;
            }
            return 0xFF;
        case 0x3F7: {
            // Digital Input Register, bit 7: disk-change, for whichever
            // drive the DOR's select bits currently point at. Genuinely
            // load-bearing: a multi-floppy installer's file-copy routine
            // polls this to confirm the user actually swapped media before
            // trusting a re-read of the drive -- reporting "unchanged"
            // unconditionally left it waiting forever for a change that
            // would never come, regardless of how many times the correct
            // new disk had already been mounted. See Drive::disk_changed
            // and IBM_PCAT_REVIEW.md.
            int drive = dor_ & 0x01;
            return drives[drive].disk_changed ? 0x80 : 0x00;
        }
        default: return 0xFF;
    }
}

void Fdc765::out(uint16_t port, uint8_t v) {
    switch (port) {
        case 0x3F2: {
            bool was_reset = !(dor_ & 0x04);
            dor_ = v;
            drives[0].motor_on = (v & 0x10) != 0;
            drives[1].motor_on = (v & 0x20) != 0;
            bool now_reset = !(v & 0x04);
            if (was_reset && !now_reset) {
                // Rising edge of ~RESET (leaving the held-reset state) --
                // real hardware raises an interrupt here.
                phase_ = Phase::kIdle;
                irq_pending_ = true;
            } else if (now_reset) {
                phase_ = Phase::kIdle;
            }
            break;
        }
        case 0x3F5:
            if (phase_ == Phase::kIdle) start_command(v);
            else if (phase_ == Phase::kCommand) {
                params_[params_received_++] = v;
                if (params_received_ >= param_count_needed_) run_command();
            }
            break;
        case 0x3F7: break;  // Configuration Control Register (data rate select) -- not modeled
        default: break;
    }
}

void Fdc765::start_command(uint8_t first_byte) {
    cmd_ = first_byte;
    uint8_t base = uint8_t(first_byte & 0x1F);
    params_received_ = 0;
    switch (base) {
        case 0x03: param_count_needed_ = 2; break;                     // SPECIFY
        case 0x04: param_count_needed_ = 1; break;                     // SENSE DRIVE STATUS
        case 0x05: case 0x06: case 0x09: case 0x0C:                    // WRITE/READ (DELETED) DATA
            param_count_needed_ = 8; break;
        case 0x07: param_count_needed_ = 1; break;                     // RECALIBRATE
        case 0x08: param_count_needed_ = 0; break;                     // SENSE INTERRUPT STATUS
        case 0x0A: param_count_needed_ = 1; break;                     // READ ID
        case 0x0D: param_count_needed_ = 5; break;                     // FORMAT TRACK
        case 0x0F: param_count_needed_ = 2; break;                     // SEEK
        default: param_count_needed_ = 0; break;                       // unknown -- no-op
    }
    if (param_count_needed_ == 0) run_command();
    else phase_ = Phase::kCommand;
}

void Fdc765::begin_transfer(bool is_write) {
    int drive = params_[0] & 1;
    int head = (params_[0] >> 2) & 1;
    int cyl = params_[1];
    int sector = params_[3];
    int eot = params_[5];

    Drive &d = drives[drive];
    int count_sectors = eot - sector + 1;
    if (count_sectors < 1) count_sectors = 1;
    transfer_len_ = std::size_t(count_sectors) * Drive::kBytesPerSector;
    xfer_offset_ = d.offset_for(cyl, head, sector);
    xfer_drive_ = drive;
    transfer_is_write_ = is_write;
    xfer_credit_ = 0.0;
    xfer_target_ = double(transfer_len_) / d.bytes_per_sec * kCpuHz;
    xfer_active_ = true;
    transfer_ready_ = false;
    phase_ = Phase::kExecution;

    result_[3] = uint8_t(cyl);
    result_[4] = uint8_t(head);
    result_[5] = uint8_t(eot);
    result_[6] = 2;  // N: 512-byte sectors
}

void Fdc765::run_command() {
    uint8_t base = uint8_t(cmd_ & 0x1F);
    switch (base) {
        case 0x03:  // SPECIFY: step-rate/head-load timings -- accepted, not used (see file header)
            phase_ = Phase::kIdle;
            break;
        case 0x04: {  // SENSE DRIVE STATUS
            int drive = params_[0] & 1;
            uint8_t st3 = uint8_t(drive) | 0x20;  // drive select bits + "ready"
            if (drives[drive].current_cylinder == 0) st3 = uint8_t(st3 | 0x10);  // track 0
            result_[0] = st3;
            result_count_ = 1; result_sent_ = 0;
            phase_ = Phase::kResult;
            break;
        }
        case 0x07: case 0x0F: {  // RECALIBRATE / SEEK -- implied-seek commands
            int drive = params_[0] & 1;
            int new_cyl = (base == 0x07) ? 0 : params_[1];
            int steps = std::abs(new_cyl - drives[drive].current_cylinder);
            drives[drive].current_cylinder = new_cyl;
            // Real hardware: DSKCHG clears once the drive actually steps --
            // this is how software confirms a floppy swap "took" before
            // trusting whatever it reads next. See Drive::disk_changed.
            drives[drive].disk_changed = false;
            seek_drive_ = drive;
            seeking_ = true;
            seek_credit_ = 0.0;
            seek_target_ = double(std::max(1, steps)) * drives[drive].cycles_per_track_step;
            phase_ = Phase::kIdle;  // command byte(s) accepted; completion signaled later via IRQ
            break;
        }
        case 0x08: {  // SENSE INTERRUPT STATUS
            if (seek_pending_irq_) {
                result_[0] = uint8_t(0x20 | seek_drive_);  // ST0: seek end
                result_[1] = uint8_t(drives[seek_drive_].current_cylinder);  // PCN
                seek_pending_irq_ = false;
            } else {
                result_[0] = 0x80;  // invalid command -- no interrupt was actually pending
                result_[1] = 0;
            }
            result_count_ = 2; result_sent_ = 0;
            phase_ = Phase::kResult;
            break;
        }
        case 0x0A: {  // READ ID
            int drive = params_[0] & 1;
            int head = (params_[0] >> 2) & 1;
            result_[0] = 0; result_[1] = 0; result_[2] = 0;
            result_[3] = uint8_t(drives[drive].current_cylinder);
            result_[4] = uint8_t(head);
            result_[5] = 1;
            result_[6] = 2;
            result_count_ = 7; result_sent_ = 0;
            phase_ = Phase::kResult;
            break;
        }
        case 0x05: case 0x09: begin_transfer(true); break;
        case 0x06: case 0x0C: begin_transfer(false); break;
        case 0x0D: {  // FORMAT TRACK -- accepted, not actually reformatted (see file header)
            for (int i = 0; i < 7; ++i) result_[i] = 0;
            result_count_ = 7; result_sent_ = 0;
            phase_ = Phase::kResult;
            break;
        }
        default:
            phase_ = Phase::kIdle;
            break;
    }
}

void Fdc765::tick(uint64_t cpu_cycles) {
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

uint8_t *Fdc765::transfer_image_ptr() {
    Drive &d = drives[xfer_drive_];
    if (xfer_offset_ < 0 || std::size_t(xfer_offset_) + transfer_len_ > d.image.size()) return nullptr;
    return d.image.data() + xfer_offset_;
}

void Fdc765::finish_transfer(std::size_t actual_len) {
    (void)actual_len;
    xfer_active_ = false;
    transfer_ready_ = false;
    result_[0] = 0;  // ST0: normal termination
    result_[1] = 0;
    result_[2] = 0;
    result_count_ = 7; result_sent_ = 0;
    phase_ = Phase::kResult;
    if (dor_ & 0x08) irq_pending_ = true;  // DMA/IRQ enable bit
    if (transfer_is_write_) drives[xfer_drive_].dirty = true;
}

}  // namespace ibmpcat
