#include "fdc765.h"

#include <algorithm>
#include <cstdlib>

namespace pc486 {

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
    // Mounted media survives a controller reset.
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
    // Geometry follows the mounted image: a 3.5" HD drive also reads 720KB
    // double-density media. Using the drive's HD geometry for a 720KB image would
    // put CHS math past the first sector on the wrong bytes, and chipset.cpp's DMA
    // path would report success with zero bytes moved.
    if (len <= 737280) {
        // 720KB: 80 cyl / 2 head / 9 sec/track, 250 kbit/s, ~3ms/track step.
        d.cylinders = 80; d.heads = 2; d.sectors_per_track = 9;
        d.bytes_per_sec = 31250.0;
        d.cycles_per_track_step = 0.003 * cpu_hz_;
    } else {
        // 1.44MB: 80 cyl / 2 head / 18 sec/track, 500 kbit/s, ~3ms/track step.
        d.cylinders = 80; d.heads = 2; d.sectors_per_track = 18;
        d.bytes_per_sec = 62500.0;
        d.cycles_per_track_step = 0.003 * cpu_hz_;
    }
}
void Fdc765::unmount(int drive) {
    Drive &d = drives[drive & 1];
    d.image.clear();
    d.present = false;
    // A 3.5" drive asserts DSKCHG when the disk is ejected and resets it
    // only on a step pulse with a disk inserted (TEAC FD-235HF spec,
    // "Disk Change"), so an empty bay keeps reporting "changed".
    d.disk_changed = true;
}

void Fdc765::set_cpu_hz(double hz) {
    if (!(hz > 0.0) || hz == cpu_hz_) return;
    const double scale = hz / cpu_hz_;
    if (seeking_) seek_target_ = seek_credit_ + (seek_target_ - seek_credit_) * scale;
    if (xfer_active_) xfer_target_ = xfer_credit_ + (xfer_target_ - xfer_credit_) * scale;
    cpu_hz_ = hz;
    for (int i = 0; i < 2; ++i) {
        if (drives[i].present) drives[i].cycles_per_track_step = 0.003 * cpu_hz_;
    }
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
                // The INT line drops when the CPU reads the first result byte (ST0), not after
                // the whole result phase. See ibmpc-at/IBM_PCAT_REVIEW.md §8.
                if (result_sent_ == 1) irq_pending_ = false;
                if (result_sent_ >= result_count_) phase_ = Phase::kIdle;
                return v;
            }
            return 0xFF;
        case 0x3F7: {
            // Digital Input Register bit 7: disk change for the DOR-selected drive.
            // Installers poll it to confirm a swap. See Drive::disk_changed.
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
                // Rising edge of ~RESET raises an interrupt.
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
    xfer_target_ = double(transfer_len_) / d.bytes_per_sec * cpu_hz_;
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
            // DSKCHG clears once the drive actually steps. See Drive::disk_changed.
            if (drives[drive].present) drives[drive].disk_changed = false;
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

}  // namespace pc486
