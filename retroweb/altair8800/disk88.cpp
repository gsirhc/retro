// MITS Altair 88-DCDD controller — logic ported from Charles E. Owen's
// altair_dsk.c (SIMH, 1997-2010). Status flags are held here as 1=true and
// returned inverted, matching the hardware's active-low sense.

#include "disk88.h"

namespace altair {

namespace {
constexpr uint8_t F_ENWD  = 0x01;   // ready to accept a write byte
constexpr uint8_t F_MOVE  = 0x02;   // head movement allowed
constexpr uint8_t F_HEAD  = 0x04;   // head is loaded
constexpr uint8_t F_INTE  = 0x20;   // interrupts enabled (unused here)
constexpr uint8_t F_TRK0  = 0x40;   // head at track 0
constexpr uint8_t F_NRDA  = 0x80;   // a read byte is available

// Function bits written to OUT 0x09.
constexpr uint8_t FN_STEP_IN   = 0x01;
constexpr uint8_t FN_STEP_OUT  = 0x02;
constexpr uint8_t FN_HEAD_LOAD = 0x04;
constexpr uint8_t FN_HEAD_UNLD = 0x08;
constexpr uint8_t FN_WRITE     = 0x80;
}  // namespace

void Disk88::reset() {
    for (Drive &d : drives_) d.track = 0;
    selected_ = -1;
    flags_ = 0;
    sector_ = -1;
    bufpos_ = 255;
    write_dirty_ = false;
}

bool Disk88::mount(int drive, const uint8_t *data, std::size_t len) {
    if (drive < 0 || drive >= kDrives) return false;
    Drive &d = drives_[drive];
    d.image.assign(kImageSize, 0);
    if (data && len) {
        const std::size_t n = len < kImageSize ? len : kImageSize;
        for (std::size_t i = 0; i < n; ++i) d.image[i] = data[i];
    }
    d.track = 0;
    d.dirty_since_mount = false;
    return true;
}

void Disk88::unmount(int drive) {
    if (drive < 0 || drive >= kDrives) return;
    if (selected_ == drive) { selected_ = -1; flags_ = 0; }
    drives_[drive].image.clear();
    drives_[drive].track = 0;
    drives_[drive].dirty_since_mount = false;
}

void Disk88::flushWrite() {
    if (!write_dirty_ || selected_ < 0) { write_dirty_ = false; return; }
    Drive &d = drives_[selected_];
    for (int i = bufpos_; i <= kSectorLen; ++i) sector_buf_[i] = 0;   // null-fill remainder
    if (!d.image.empty() && d.track >= 0 && d.track < kTracks &&
        sector_ >= 0 && sector_ < kSectors) {
        const std::size_t pos = std::size_t(d.track) * kSectors * kSectorLen +
                                std::size_t(sector_) * kSectorLen;
        for (int i = 0; i < kSectorLen; ++i) d.image[pos + i] = sector_buf_[i];
        d.dirty_since_mount = true;
    }
    flags_ &= ~F_ENWD;
    bufpos_ = 255;
    write_dirty_ = false;
}

uint8_t Disk88::in(uint8_t port) {
    switch (port) {
        case 0x08: {                                   // controller/drive status
            if (selected_ < 0) return 0xFF;            // nothing selected: all-false
            return static_cast<uint8_t>(~flags_ & 0xFF);
        }

        case 0x09: {                                   // sector position register
            flushWrite();
            if (selected_ < 0 || !(flags_ & F_HEAD)) return 0;   // head not loaded
            if (++sector_ > 31) sector_ = 0;
            bufpos_ = 255;
            uint8_t stat = static_cast<uint8_t>((sector_ << 1) & 0x3E);
            stat |= 0xC0;                               // unused hi bits read 1; bit0 (T)=0 => "true"
            return stat;
        }

        case 0x0A: {                                   // read data byte
            if (selected_ < 0) return 0;
            Drive &d = drives_[selected_];
            if (bufpos_ < kSectorLen + 1) {
                ++io_ticks_;
                return sector_buf_[bufpos_++];
            }
            // Fill the buffer from the current track/sector.
            for (int i = 0; i <= kSectorLen; ++i) sector_buf_[i] = 0;
            if (!d.image.empty() && d.track >= 0 && d.track < kTracks &&
                sector_ >= 0 && sector_ < kSectors) {
                const std::size_t pos = std::size_t(d.track) * kSectors * kSectorLen +
                                        std::size_t(sector_) * kSectorLen;
                for (int i = 0; i < kSectorLen; ++i) sector_buf_[i] = d.image[pos + i];
            }
            bufpos_ = 1;
            ++io_ticks_;
            return sector_buf_[0];
        }
    }
    return 0xFF;
}

void Disk88::out(uint8_t port, uint8_t value) {
    switch (port) {
        case 0x08: {                                   // select / enable
            flushWrite();
            const int drive = value & 0x0F;
            selected_ = drive;
            sector_ = -1;
            bufpos_ = 255;
            if (value & 0x80) {                        // deselect / disable
                selected_ = -1;
                flags_ = 0;
                return;
            }
            flags_ = F_MOVE | 0x08 | 0x10;             // enable; head-move allowed (SIMH 0x1A)
            if (drives_[drive].track == 0) flags_ |= F_TRK0;
            return;
        }

        case 0x09: {                                   // drive function
            if (selected_ < 0) return;
            flushWrite();                              // commit any pending write before we move
            Drive &d = drives_[selected_];
            if (value & FN_STEP_IN) {
                if (++d.track > 76) d.track = 76;
                sector_ = -1; bufpos_ = 255;
                ++step_ticks_;
            }
            if (value & FN_STEP_OUT) {
                if (--d.track < 0) d.track = 0;
                sector_ = -1; bufpos_ = 255;
                ++step_ticks_;
            }
            // The track-0 line is a physical sensor: keep it honest rather than
            // latch it like SIMH does (a stale flag breaks BIOSes whose head-home
            // routine trusts it, e.g. Burcon CP/M's seek0).
            if (d.track == 0) flags_ |= F_TRK0; else flags_ &= ~F_TRK0;
            if (value & FN_HEAD_LOAD) flags_ |= F_HEAD | F_NRDA;
            if (value & FN_HEAD_UNLD) {
                flags_ &= ~(F_HEAD | F_NRDA);
                sector_ = -1; bufpos_ = 255;
            }
            if (value & FN_WRITE) {                    // begin a 137-byte write
                bufpos_ = 0;
                write_dirty_ = false;
                flags_ |= F_ENWD;
            }
            (void)F_INTE;
            return;
        }

        case 0x0A: {                                   // write data byte
            if (selected_ < 0) return;
            if (bufpos_ > 136) {
                if (bufpos_ <= kSectorLen) sector_buf_[bufpos_] = value;
                flushWrite();
            } else {
                sector_buf_[bufpos_++] = value;
                write_dirty_ = true;
                ++io_ticks_;
            }
            return;
        }
    }
}

} // namespace altair
