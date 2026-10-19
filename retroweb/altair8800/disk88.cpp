// MITS 88-DCDD controller (MITS 88-DCDD manual pp.133-136, controller schematics)
// driving Pertec FD-400 drives (Pertec 600500 manual, Table 1-1 and §2.6.3).

#include "disk88.h"

#include <algorithm>
#include <cstring>

namespace altair {

namespace {

// Disk time runs in sixths of a microsecond, three per 2 MHz CPU cycle.
constexpr int64_t kTicksPerCycle = 3;
constexpr int64_t kUs = 6;
constexpr int64_t kMs = 1000 * kUs;

constexpr int64_t kSectorTicks  = 1000000 / Disk88::kSectors;   // 166.67 ms rev at 360 rpm
constexpr int64_t kSectorTrue   = 30 * kUs;                     // SR0 pulse (p.136)
constexpr int64_t kFirstRead    = 140 * kUs;                    // read data after SR0 (p.136)
constexpr int64_t kFirstWrite   = 280 * kUs;                    // first ENWD after SR0 (p.134)
constexpr int64_t kByteTicks    = 32 * kUs;                     // 250 kbit/s
constexpr int64_t kStepInhibit1 = 10 * kMs;                     // move head false/true/false (p.134)
constexpr int64_t kStepOk       = 1 * kMs;
constexpr int64_t kStepInhibit2 = 20 * kMs;
constexpr int64_t kHeadSettle   = 40 * kMs;                     // HS and move head after load (p.134)
constexpr int64_t kTrimEnd      = 475 * kUs;                    // trim erase after write (p.135)
constexpr int64_t kTrack0Hold   = 40 * kMs;                     // FD-400 track 0 masked after a step

constexpr uint8_t ST_ENWD  = 0x01;
constexpr uint8_t ST_MOVE  = 0x02;
constexpr uint8_t ST_HEAD  = 0x04;
constexpr uint8_t ST_INTE  = 0x20;
constexpr uint8_t ST_TRK0  = 0x40;
constexpr uint8_t ST_NRDA  = 0x80;

constexpr uint8_t CTL_STEP_IN  = 0x01;
constexpr uint8_t CTL_STEP_OUT = 0x02;
constexpr uint8_t CTL_LOAD     = 0x04;
constexpr uint8_t CTL_UNLOAD   = 0x08;
constexpr uint8_t CTL_WRITE    = 0x80;

}  // namespace

void Disk88::reset() {
    clear();
}

void Disk88::clear() {
    if (w_active_) {
        w_active_ = false;
        write_end_at_ = now_;
    }
    selected_ = -1;
    head_loaded_ = false;
    u_valid_ = false;
    xfer_sector_ = -1;
}

void Disk88::setSpeed(int sectorsPerSec) {
    if (w_active_) {
        w_active_ = false;
        write_end_at_ = now_;
    }
    rate_ = sectorsPerSec > 0 ? sectorsPerSec : 0;
    has_last_ = false;
    has_pending_ = false;
    u_valid_ = false;
}

void Disk88::tick(uint64_t cpuCycles) {
    clock(cpuCycles);
    advance(false);
}

void Disk88::advance(bool scalable) {
    if (!timed() || !has_pending_) return;
    has_pending_ = false;
    const uint64_t cy = pending_cy_;
    if (!has_last_ || cy < last_cy_) {
        last_cy_ = cy;
        has_last_ = true;
        return;
    }
    const int64_t dt = int64_t(cy - last_cy_) * kTicksPerCycle;
    last_cy_ = cy;
    const double k = rate_ / kRealSectorsPerSec;
    if (scalable && k > 1.0 && !in_transfer()) {
        const int64_t cap = next_event();
        const double scaled = double(dt) * k;
        move_to(scaled >= double(cap - now_) ? cap : now_ + int64_t(scaled));
    } else {
        move_to(now_ + dt);
    }
}

void Disk88::move_to(int64_t target) {
    while (w_active_) {
        const int64_t end  = (w_sector_ + 1) * kSectorTicks;
        const int64_t load = w_first_ + kByteTicks * (w_slot_ + 1);
        if (load < end && load <= target) {
            put(w_drive_, drives_[w_drive_].track, int(w_sector_ % kSectors), w_slot_, w_latch_);
            ++w_slot_;
            w_fresh_ = false;
            continue;
        }
        if (target >= end) {
            w_active_ = false;
            write_end_at_ = end;
        }
        break;
    }
    now_ = target;
}

bool Disk88::in_transfer() const {
    return w_active_ || xfer_sector_ == now_ / kSectorTicks;
}

int64_t Disk88::next_event() const {
    const int64_t start = (now_ / kSectorTicks) * kSectorTicks;
    if (enabled() && head_loaded_ && now_ < start + kFirstRead) return start + kFirstRead;
    return start + kSectorTicks;
}

int Disk88::read_index() const {
    if (!enabled() || !head_loaded_ || w_active_) return -1;
    const int64_t off = now_ - (now_ / kSectorTicks) * kSectorTicks - kFirstRead;
    return off < 0 ? -1 : int(off / kByteTicks);
}

bool Disk88::nrda() const {
    if (!timed()) return enabled() && head_loaded_ && u_valid_ && !w_active_;
    const int idx = read_index();
    if (idx < 0) return false;
    return !(rd_sector_ == now_ / kSectorTicks && rd_index_ >= idx);
}

bool Disk88::move_ok() const {
    if (!timed()) return true;
    if (now_ < head_mh_until_) return false;
    const int64_t d = now_ - step_at_;
    if (d < kStepInhibit1) return false;
    if (d >= kStepInhibit1 + kStepOk && d < kStepInhibit1 + kStepOk + kStepInhibit2) return false;
    return !w_active_ && now_ >= write_end_at_ + kTrimEnd;
}

bool Disk88::head_ready() const {
    return head_loaded_ && (!timed() || now_ >= settle_until_);
}

bool Disk88::track0() const {
    const Drive& d = drives_[selected_];
    return d.track == 0 && (!timed() || now_ >= d.moved_at + kTrack0Hold);
}

uint8_t Disk88::get(int drive, int track, int sector, int pos) const {
    const std::vector<uint8_t>& img = drives_[drive].image;
    if (img.empty()) return 0xFF;
    if (pos >= kSectorLen) return 0x00;   // the 138th byte written is 000 to sector end (p.135)
    return img[(std::size_t(track) * kSectors + sector) * kSectorLen + pos];
}

void Disk88::put(int drive, int track, int sector, int pos, uint8_t v) {
    std::vector<uint8_t>& img = drives_[drive].image;
    if (img.empty() || pos >= kSectorLen) return;
    img[(std::size_t(track) * kSectors + sector) * kSectorLen + pos] = v;
    drives_[drive].dirty_since_mount = true;
}

void Disk88::step(bool in) {
    Drive& d = drives_[selected_];
    if (timed()) {
        step_at_ = now_;
        settle_until_ = now_ + kHeadSettle;
    } else {
        end_write(true);
        u_valid_ = false;
    }
    const int t = std::clamp(d.track + (in ? 1 : -1), 0, kTracks - 1);
    if (t != d.track) {
        d.track = t;
        d.moved_at = now_;
    }
    ++step_ticks_;
}

void Disk88::start_write() {
    if (w_active_) return;
    if (timed()) {
        const int64_t s = now_ / kSectorTicks;
        w_sector_ = s;
        w_first_ = std::max(s * kSectorTicks + kFirstWrite, now_);
        xfer_sector_ = s;
    } else {
        if (!u_valid_) return;
        w_sector_ = u_sector_;
    }
    w_active_ = true;
    w_drive_ = selected_;
    w_slot_ = 0;
    w_fresh_ = false;
    w_any_ = false;
}

void Disk88::end_write(bool fill) {
    if (!w_active_) return;
    if (fill && w_any_)
        for (; w_slot_ < kSectorLen; ++w_slot_)
            put(w_drive_, drives_[w_drive_].track, int(w_sector_), w_slot_, w_latch_);
    w_active_ = false;
}

uint8_t Disk88::in(uint8_t port) {
    switch (port) {
    case 0x08: {
        advance(true);
        if (!enabled()) return 0xFF;
        const bool ready = nrda();
        if (ready && timed()) xfer_sector_ = now_ / kSectorTicks;
        const bool enwd = w_active_ && (!timed() || (now_ >= w_first_ && !w_fresh_));
        uint8_t st = 0;
        if (!enwd)         st |= ST_ENWD;
        if (!move_ok())    st |= ST_MOVE;
        if (!head_ready()) st |= ST_HEAD;
        if (!inte_)        st |= ST_INTE;
        if (!track0())     st |= ST_TRK0;
        if (!ready)        st |= ST_NRDA;
        return st;
    }
    case 0x09: {
        advance(true);
        if (!enabled() || !head_ready()) return 0xFF;
        if (!timed()) {
            end_write(true);
            u_sector_ = (u_sector_ + 1) % kSectors;
            u_valid_ = true;
            u_pos_ = 0;
            return uint8_t(0xC0 | (u_sector_ << 1));
        }
        const int64_t s = now_ / kSectorTicks;
        const bool sector_true = now_ - s * kSectorTicks < kSectorTrue;
        return uint8_t(0xC0 | ((s % kSectors) << 1) | (sector_true ? 0 : 1));
    }
    case 0x0A: {
        advance(false);
        if (!enabled()) return 0xFF;
        if (!timed()) {
            if (nrda()) {
                rd_latch_ = get(selected_, drives_[selected_].track, u_sector_, u_pos_);
                if (u_pos_ < kSectorLen) ++u_pos_;
            }
        } else {
            const int idx = read_index();
            if (idx >= 0) {
                const int64_t s = now_ / kSectorTicks;
                if (rd_sector_ != s || rd_index_ != idx)
                    rd_latch_ = get(selected_, drives_[selected_].track, int(s % kSectors), idx);
                rd_sector_ = s;
                rd_index_ = idx;
                xfer_sector_ = s;
            }
        }
        ++io_ticks_;
        return rd_latch_;
    }
    }
    return 0xFF;
}

void Disk88::out(uint8_t port, uint8_t value) {
    advance(false);
    switch (port) {
    case 0x08: {
        if (value & 0x80) { clear(); return; }
        const int d = value & 0x0F;
        if (!mounted(d)) { clear(); return; }   // no media: the drive can't be enabled (p.133)
        if (d != selected_) u_valid_ = false;
        selected_ = d;
        return;
    }
    case 0x09: {
        if (!enabled()) return;
        if (value & CTL_STEP_IN) step(true);
        else if (value & CTL_STEP_OUT) step(false);
        const bool load = value & CTL_LOAD, unload = value & CTL_UNLOAD;
        if (load && unload) head_loaded_ = !head_loaded_;
        else if (load) head_loaded_ = true;
        else if (unload) head_loaded_ = false;
        if (load && timed()) {
            settle_until_ = now_ + kHeadSettle;
            head_mh_until_ = now_ + kHeadSettle;
        }
        if (value & CTL_WRITE) start_write();
        return;
    }
    case 0x0A: {
        if (!enabled()) return;
        if (!timed() && w_active_) {
            put(w_drive_, drives_[w_drive_].track, int(w_sector_), w_slot_, value);
            if (w_slot_ < kSectorLen) ++w_slot_;
        }
        w_latch_ = value;
        w_fresh_ = true;
        w_any_ = true;
        ++io_ticks_;
        return;
    }
    }
}

bool Disk88::mount(int drive, const uint8_t* data, std::size_t len) {
    if (drive < 0 || drive >= kDrives) return false;
    if (selected_ == drive) clear();
    if (w_active_ && w_drive_ == drive) w_active_ = false;
    Drive& d = drives_[drive];
    d.image.assign(kImageSize, 0x00);
    if (data && len) std::memcpy(d.image.data(), data, std::min(len, kImageSize));
    d.dirty_since_mount = false;
    return true;
}

void Disk88::unmount(int drive) {
    if (drive < 0 || drive >= kDrives) return;
    if (selected_ == drive) clear();
    if (w_active_ && w_drive_ == drive) w_active_ = false;
    drives_[drive].image.clear();
    drives_[drive].dirty_since_mount = false;
}

} // namespace altair
