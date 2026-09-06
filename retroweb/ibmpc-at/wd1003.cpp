#include "wd1003.h"

#include <algorithm>
#include <cstring>

namespace ibmpcat {

void Wd1003::reset() {
    error_ = 0;
    sector_count_ = 1;
    sector_number_ = 1;
    cyl_low_ = 0;
    cyl_high_ = 0;
    drive_head_ = 0xA0;
    status_ = ST_DRDY | ST_DSC;
    floating_reads_left_ = 0;
    nien_ = false;
    srst_prev_ = false;
    selected_drive_ = 0;
    irq_pending_ = false;
    pio_buffer_.clear();
    pio_pos_ = 0;
    pio_mode_ = PioMode::kNone;
    xfer_active_ = false;
    xfer_is_write_ = false;
    xfer_credit_ = xfer_target_ = 0.0;
    prev_cycles_ = 0;
    // drives[] deliberately survive reset -- a real fixed disk's contents
    // aren't erased by a CPU/controller reset.
}

void Wd1003::mount(int drive, const uint8_t *data, std::size_t len) {
    Drive &d = drives[drive & 1];
    d.image.assign(data, data + len);
    d.present = true;
    d.dirty = false;
    // ST-4038 geometry -- this system's only fixed-disk configuration.
    d.cylinders = 733;
    d.heads = 5;
    d.sectors_per_track = 17;
}

bool Wd1003::owns(uint16_t port) const {
    return (port >= 0x1F0 && port <= 0x1F7) || port == 0x3F6;
}

uint8_t Wd1003::pio_read_byte() {
    if (pio_mode_ != PioMode::kReadDrain || pio_pos_ >= pio_buffer_.size()) return 0xFF;
    uint8_t b = pio_buffer_[pio_pos_++];
    if (pio_pos_ >= pio_buffer_.size()) {
        pio_mode_ = PioMode::kNone;
        status_ = uint8_t((status_ & uint8_t(~(ST_DRQ | ST_BSY))) | ST_DRDY | ST_DSC);
    }
    return b;
}
void Wd1003::pio_write_byte(uint8_t v) {
    if (pio_mode_ != PioMode::kWriteFill || pio_pos_ >= pio_buffer_.size()) return;
    pio_buffer_[pio_pos_++] = v;
    if (pio_pos_ >= pio_buffer_.size()) {
        // Commit synchronously the instant the CPU has supplied the whole
        // sector, rather than through the tick()-paced xfer_credit_ delay
        // READ SECTORS uses. This is a real ATA asymmetry, not an
        // inconsistency: a genuine drive absorbs incoming write data into
        // a small buffer and can report completion almost immediately
        // (nothing to wait for -- the data's already in hand), unlike a
        // read, which can't start moving anything until it's actually been
        // fetched off the platter first. This machine's real firmware
        // substitute hard-codes exactly that assumption: rombios.c's
        // ata_cmd_data_io() calls await_ide() before its read-side
        // completion check but *not* before its write-side one -- it reads
        // status back immediately after the last `outsw` word and requires
        // BSY to already be clear right then. Confirmed by disassembling
        // its actual write path after every real WRITE SECTORS this
        // controller completed correctly was still reported as a failure
        // by the BIOS. See IBM_PCAT_REVIEW.md.
        pio_mode_ = PioMode::kNone;
        finish_read_or_write();
    }
}

uint16_t Wd1003::data_in16() {
    uint8_t lo = pio_read_byte();
    uint8_t hi = pio_read_byte();
    return uint16_t(lo) | (uint16_t(hi) << 8);
}
void Wd1003::data_out16(uint16_t v) {
    pio_write_byte(uint8_t(v & 0xFF));
    pio_write_byte(uint8_t(v >> 8));
}

uint8_t Wd1003::in(uint16_t port) {
    // Reads are never gated on which drive is *currently* selected -- see
    // selected_drive_absent()'s comment for why: on this machine's real
    // single-device cable, Device 0 answers register reads regardless of
    // the DEV bit when no Device 1 is wired at all, since there's nothing
    // else on the bus to float to. (Writes to the command register ARE
    // gated -- see out().) The one exception is floating_reads_left_'s
    // brief post-reset detection window -- see its comment.
    if ((port == 0x1F4 || port == 0x1F5) && floating_reads_left_ > 0) {
        --floating_reads_left_;
        return 0xFF;
    }
    switch (port) {
        case 0x1F0: return pio_read_byte();
        case 0x1F1: return error_;
        case 0x1F2: return uint8_t(sector_count_);
        case 0x1F3: return sector_number_;
        case 0x1F4: return cyl_low_;
        case 0x1F5: return cyl_high_;
        case 0x1F6: return drive_head_;
        case 0x1F7: irq_pending_ = false; return status_;  // reading status acknowledges the interrupt
        case 0x3F6: return status_;  // alternate status: same bits, does NOT clear the interrupt
        default: return 0xFF;
    }
}

void Wd1003::out(uint16_t port, uint8_t v) {
    // Only the COMMAND register write (0x1F7) is gated on drive selection.
    // Real ATA/IDE task-file registers other than the command register
    // (Features/Sector Count/Sector Number/Cylinder Low/Cylinder High) are
    // simply latches on a shared parallel bus, written by whichever real
    // device is physically present regardless of the Drive/Head register's
    // DEV bit -- there's no per-device routing for them at all, since
    // that's not how the bus works. Only the command register write
    // actually depends on which device is "selected", because that's what
    // decides which device's command-execution logic responds. Getting
    // this backwards -- gating SC/SN/CL/CH/FR too -- was a real bug caught
    // by a genuine HDD-only boot test: this BIOS programs those registers
    // *before* reselecting the master (right after probing the slave
    // last during POST), so gating them on the then-still-selected slave
    // silently discarded the real CHS parameters, corrupting the very
    // first boot-sector read into a bogus, out-of-range request.
    // selected_drive_absent()'s comment covers 0x1F7 and 0x3F6 in detail.
    if (port == 0x1F7 && selected_drive_absent()) return;
    switch (port) {
        case 0x1F0: pio_write_byte(v); break;
        case 0x1F1: error_ = v; break;  // "features" register when written; not interpreted
        case 0x1F2: sector_count_ = v; break;
        case 0x1F3: sector_number_ = v; break;
        case 0x1F4: cyl_low_ = v; break;
        case 0x1F5: cyl_high_ = v; break;
        case 0x1F6: drive_head_ = v; selected_drive_ = (v & 0x10) ? 1 : 0; break;
        case 0x1F7: run_command(v); break;
        case 0x3F6: {
            bool srst_now = (v & 0x04) != 0;
            if (srst_now && !srst_prev_) {
                // Soft reset: the one real device on this cable always
                // reasserts its own genuine post-reset signature (sector
                // count/number = 1, cylinder = 0, status DRDY|DSC) --
                // that's a hardware fact about the master, not something
                // that depends on what the DEV bit happens to claim is
                // selected. But if SRST was released while the *absent*
                // slave was the one selected, the real BIOS's ata_detect()
                // needs to see Cylinder Low/High float to 0xFF/0xFF right
                // afterward to conclude "no second drive" -- that's
                // floating_reads_left_'s job (see its comment): it makes
                // exactly the next two reads of those two registers show
                // the floated value, matching ata_detect()'s own two reads
                // (Cylinder Low then High) immediately following reset,
                // without permanently corrupting the real device's actual
                // signature for anything that reads it afterward (a real
                // bug this session's HDD-only boot test caught: the boot
                // loader reads status well after this detection window
                // with the slave still nominally selected, and needs the
                // real master value, not a stuck floating one).
                sector_count_ = 1;
                sector_number_ = 1;
                cyl_low_ = 0x00;
                cyl_high_ = 0x00;
                status_ = ST_DRDY | ST_DSC;
                floating_reads_left_ = selected_drive_absent() ? 2 : 0;
                error_ = 0;
                pio_mode_ = PioMode::kNone;
                xfer_active_ = false;
            }
            srst_prev_ = srst_now;
            nien_ = (v & 0x02) != 0;
            break;
        }
        default: break;
    }
}

void Wd1003::run_command(uint8_t cmd) {
    error_ = 0;
    if ((cmd & 0xF0) == 0x10) {  // RECALIBRATE (0x10-0x1F)
        status_ = ST_DRDY | ST_DSC;
        irq_pending_ = !nien_;
        return;
    }
    switch (cmd) {
        case 0xEC: do_identify(); break;
        case 0x91:  // INITIALIZE DEVICE PARAMETERS -- accepted; this system's geometry is already fixed
            status_ = ST_DRDY | ST_DSC;
            irq_pending_ = !nien_;
            break;
        case 0x20: case 0x21: begin_read(); break;
        case 0x30: case 0x31: begin_write(); break;
        default:
            status_ = ST_DRDY | ST_DSC | ST_ERR;
            error_ = 0x04;  // ABRT
            irq_pending_ = !nien_;
            break;
    }
}

void Wd1003::do_identify() {
    Drive &d = drives[selected_drive_];
    pio_buffer_.assign(512, 0);

    auto put16 = [&](int word_idx, uint16_t v) {
        pio_buffer_[std::size_t(word_idx) * 2] = uint8_t(v & 0xFF);
        pio_buffer_[std::size_t(word_idx) * 2 + 1] = uint8_t(v >> 8);
    };
    // ATA string fields store character pairs byte-swapped within each
    // 16-bit word -- a real, documented convention, not an accident.
    auto put_string = [&](int word_start, int word_count, const char *s) {
        int len = int(std::strlen(s));
        for (int i = 0; i < word_count * 2; ++i) {
            char c = (i < len) ? s[i] : ' ';
            std::size_t byte_idx = std::size_t(word_start) * 2 + std::size_t(i ^ 1);
            if (byte_idx < pio_buffer_.size()) pio_buffer_[byte_idx] = uint8_t(c);
        }
    };

    put16(0, 0x0040);  // general config: fixed (non-removable) ATA device
    put16(1, uint16_t(d.cylinders));
    put16(3, uint16_t(d.heads));
    // Word 5, "number of unformatted bytes per physical sector" -- an
    // obsolete field on any modern drive (always 512 in practice) but this
    // BIOS actually reads it back out of its own cached IDENTIFY copy to
    // size its PIO transfer loop's word count per chunk (rombios.c:
    // ata_detect() -> EbdaData->ata.devices[].blksize -> ata_cmd_data_io()'s
    // `rep insw` CX). Leaving this at the buffer's default zero-fill made
    // the BIOS's own transfer loop move zero words per "sector" -- our
    // device's paced READ SECTORS still completed and had real data
    // sitting in DRQ, but the BIOS never actually drained it, so its own
    // post-transfer completion check (expecting DRQ to have cleared) saw it
    // still set and reported the whole operation as failed. This is what
    // broke the FreeDOS installer's auto-partition step, which read genuine
    // sector data (LBA 0, offset 0) successfully at the device level yet
    // was told AH=0x0C/CF=1 by the BIOS. See IBM_PCAT_REVIEW.md.
    put16(5, Drive::kBytesPerSector);
    put16(6, uint16_t(d.sectors_per_track));
    put_string(10, 10, "0");                  // serial number
    put_string(23, 4, "1.0");                  // firmware revision
    put_string(27, 20, "IBM WD1003 ST-4038");  // model number
    long total = d.capacity_sectors();
    put16(60, uint16_t(total & 0xFFFF));
    put16(61, uint16_t((total >> 16) & 0xFFFF));
    // Word 83 bit 10 (LBA48) deliberately left 0 -- not supported, matching
    // a genuine period drive; the BIOS falls back to reading words 60/61.

    pio_pos_ = 0;
    pio_mode_ = PioMode::kReadDrain;
    status_ = ST_DRQ | ST_DRDY | ST_DSC;
    irq_pending_ = !nien_;
}

long Wd1003::offset_for_current_registers() const {
    const Drive &d = drives[selected_drive_];
    // Bit 6 of the Drive/Head register (ATA's "use LBA" convention, bit
    // value 0x40) selects 28-bit LBA addressing over classic CHS: sector
    // number = LBA[7:0], cylinder low/high = LBA[15:8]/[23:16], and the
    // head field's low nibble = LBA[27:24]. A genuine 1984 WD1003 predates
    // LBA (an ATA-2/1996-era addition) entirely -- same kind of compatibility
    // concession as IDENTIFY DEVICE above -- but it's load-bearing here:
    // this BIOS's ata_cmd_data_io() (rombios.c) always converts CHS to LBA
    // in software and issues every read/write in LBA mode, never plain CHS.
    // Missing this was a real bug: it happened to still work for LBA
    // sector 0 (coincides with CHS(0,0,1)'s offset regardless of geometry),
    // masking the problem until the FreeDOS installer's auto-partition step
    // tried to touch a sector where the two addressings genuinely diverge
    // and got a bogus offset. See IBM_PCAT_REVIEW.md.
    if (drive_head_ & 0x40) {
        uint32_t lba = uint32_t(sector_number_) | (uint32_t(cyl_low_) << 8) |
                       (uint32_t(cyl_high_) << 16) | (uint32_t(drive_head_ & 0x0F) << 24);
        return long(lba) * Drive::kBytesPerSector;
    }
    int cyl = int(cyl_low_) | (int(cyl_high_) << 8);
    int head = drive_head_ & 0x0F;
    return d.offset_for(cyl, head, sector_number_);
}

void Wd1003::begin_read() {
    int count = sector_count_ == 0 ? 256 : sector_count_;
    xfer_len_ = std::size_t(count) * Drive::kBytesPerSector;
    xfer_offset_ = offset_for_current_registers();
    xfer_is_write_ = false;
    xfer_active_ = true;
    xfer_credit_ = 0.0;
    xfer_target_ = double(xfer_len_) / kBytesPerSec * kCpuHz;
    status_ = ST_BSY;
}

void Wd1003::begin_write() {
    int count = sector_count_ == 0 ? 256 : sector_count_;
    xfer_len_ = std::size_t(count) * Drive::kBytesPerSector;
    xfer_offset_ = offset_for_current_registers();
    xfer_is_write_ = true;
    pio_buffer_.assign(xfer_len_, 0);
    pio_pos_ = 0;
    pio_mode_ = PioMode::kWriteFill;
    // Ready for the CPU to start feeding data immediately -- real ATA
    // WRITE SECTORS asserts DRQ right away rather than waiting for a seek.
    // Completion itself is synchronous once pio_write_byte() sees the last
    // byte supplied -- see its comment for why writes and reads pace
    // differently here.
    status_ = ST_DRQ | ST_DRDY;
    xfer_active_ = false;
}

void Wd1003::finish_read_or_write() {
    Drive &d = drives[selected_drive_];
    xfer_active_ = false;
    if (xfer_offset_ < 0 || std::size_t(xfer_offset_) + xfer_len_ > d.image.size()) {
        status_ = ST_DRDY | ST_DSC | ST_ERR;
        error_ = 0x10;  // IDNF (ID not found) -- closest real error code for an out-of-range CHS request
        irq_pending_ = !nien_;
        return;
    }
    if (xfer_is_write_) {
        std::copy(pio_buffer_.begin(), pio_buffer_.end(), d.image.begin() + xfer_offset_);
        d.dirty = true;
        status_ = ST_DRDY | ST_DSC;
        irq_pending_ = !nien_;
    } else {
        pio_buffer_.assign(d.image.begin() + xfer_offset_, d.image.begin() + xfer_offset_ + long(xfer_len_));
        pio_pos_ = 0;
        pio_mode_ = PioMode::kReadDrain;
        status_ = ST_DRQ | ST_DRDY | ST_DSC;
        irq_pending_ = !nien_;
    }
}

void Wd1003::tick(uint64_t cpu_cycles) {
    uint64_t d = cpu_cycles - prev_cycles_;
    prev_cycles_ = cpu_cycles;
    if (xfer_active_) {
        xfer_credit_ += double(d);
        if (xfer_credit_ >= xfer_target_) finish_read_or_write();
    }
}

}  // namespace ibmpcat
