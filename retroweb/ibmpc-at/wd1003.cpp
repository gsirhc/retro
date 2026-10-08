#include "wd1003.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace ibmpcat {

namespace {

// x^32+x^28+x^26+x^19+x^17+x^10+x^6+x^2+1, shifted MSB first (WD11C00-13 data sheet)
constexpr uint32_t kEccPoly = 0x140A0445;
// FFFFFFFF preset after the A1 F8 data mark (WD11C00-13 data sheet, Preset Generator)
constexpr uint32_t kEccSeed = 0xB517894A;
constexpr int kRecordBits = (512 + 4) * 8;
constexpr int kCorrectionSpan = 5;  // WD1003-WA2 OEM manual 5.2.3

uint32_t ecc_shift(uint32_t reg, const uint8_t* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        for (int b = 7; b >= 0; --b) {
            bool fb = ((reg >> 31) ^ (p[i] >> b)) & 1;
            reg <<= 1;
            if (fb) reg ^= kEccPoly;
        }
    }
    return reg;
}

uint32_t mul_x(uint32_t r) { return (r << 1) ^ ((r >> 31) ? kEccPoly : 0); }

// Syndrome of a single bit error at each bit of the 516-byte record.
const std::array<uint32_t, kRecordBits>& bit_syndromes() {
    static const std::array<uint32_t, kRecordBits> table = [] {
        std::array<uint32_t, kRecordBits> t{};
        const uint8_t one = 1;
        t[kRecordBits - 1] = ecc_shift(0, &one, 1);
        for (int k = kRecordBits - 2; k >= 0; --k) t[std::size_t(k)] = mul_x(t[std::size_t(k) + 1]);
        return t;
    }();
    return table;
}

// Finds a burst of up to kCorrectionSpan bits with this syndrome and flips it in data.
bool correct_burst(uint32_t syndrome, uint8_t* data) {
    const auto& t = bit_syndromes();
    for (int k = 0; k < kRecordBits; ++k) {
        for (int tail = 0; tail < (1 << (kCorrectionSpan - 1)); ++tail) {
            uint32_t s = t[std::size_t(k)];
            for (int j = 1; j < kCorrectionSpan; ++j)
                if ((tail >> (j - 1)) & 1) s ^= k + j < kRecordBits ? t[std::size_t(k + j)] : 0;
            if (s != syndrome) continue;
            for (int j = 0; j < kCorrectionSpan; ++j) {
                int bit = k + j;
                if ((j == 0 || ((tail >> (j - 1)) & 1)) && bit < 512 * 8) data[bit / 8] ^= uint8_t(0x80 >> (bit % 8));
            }
            return true;
        }
    }
    return false;
}

}  // namespace

uint32_t Wd1003::data_ecc(const uint8_t* data) { return ecc_shift(kEccSeed, data, 512); }

void Wd1003::reset() {
    error_ = 0;
    precomp_ = 0;
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
    xfer_ = Xfer::kNone;
    xfer_active_ = false;
    sectors_left_ = 0;
    long_ = no_retry_ = id_missing_ = corrected_ = false;
    retries_ = 0;
    due_ = 0.0;
    prev_cycles_ = 0;
    // drives[] survive reset.
}

void Wd1003::mount(int drive, const uint8_t *data, std::size_t len) {
    Drive &d = drives[drive & 1];
    d.image.assign(data, data + len);
    d.present = true;
    d.dirty = false;
    d.bad_sectors.clear();
    d.ecc_override.clear();
    d.layout.clear();
    // ST-4038 geometry, the only configuration
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
        if (xfer_ == Xfer::kRead && sectors_left_ > 0 && error_ == 0) {
            seek_sector();
        } else {
            xfer_ = Xfer::kNone;
            status_ = uint8_t(ST_DRDY | ST_DSC | (error_ ? ST_ERR : 0));
        }
    }
    return b;
}
void Wd1003::pio_write_byte(uint8_t v) {
    if (pio_mode_ != PioMode::kWriteFill || pio_pos_ >= pio_buffer_.size()) return;
    pio_buffer_[pio_pos_++] = v;
    if (pio_pos_ >= pio_buffer_.size()) {
        pio_mode_ = PioMode::kNone;
        if (xfer_ == Xfer::kFormat) {
            // the format starts at the index pulse and runs one revolution
            double now = double(prev_cycles_);
            due_ = now + (kCyclesPerRev - std::fmod(now, kCyclesPerRev)) + kCyclesPerRev;
            xfer_active_ = true;
            status_ = ST_BSY;
        } else {
            finish_write();
        }
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
    // Reads ignore the selected drive: Device 0 answers on this single-device
    // cable. Only the 0x1F4/5 post-reset window floats (floating_reads_left_).
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
        case 0x3F6: return status_;  // alternate status does not clear the interrupt
        default: return 0xFF;
    }
}

void Wd1003::out(uint16_t port, uint8_t v) {
    // Only the command register (0x1F7) is gated on drive selection; other
    // task-file registers are shared latches (see selected_drive_absent()).
    if (port == 0x1F7 && selected_drive_absent()) return;
    switch (port) {
        case 0x1F0: pio_write_byte(v); break;
        case 0x1F1: precomp_ = v; break;
        case 0x1F2: sector_count_ = v; break;
        case 0x1F3: sector_number_ = v; break;
        case 0x1F4: cyl_low_ = v; break;
        case 0x1F5: cyl_high_ = v; break;
        case 0x1F6: drive_head_ = v; selected_drive_ = (v & 0x10) ? 1 : 0; break;
        case 0x1F7: run_command(v); break;
        case 0x3F6: {
            bool srst_now = (v & 0x04) != 0;
            if (srst_now && !srst_prev_) {
                // Soft reset reasserts the master's signature. If SRST was released with the
                // absent slave selected, ata_detect() must see Cylinder Low/High read 0xFF for
                // its next two reads (floating_reads_left_). A permanent float broke the boot
                // loader's later status reads.
                sector_count_ = 1;
                sector_number_ = 1;
                cyl_low_ = 0x00;
                cyl_high_ = 0x00;
                status_ = ST_DRDY | ST_DSC;
                floating_reads_left_ = selected_drive_absent() ? 2 : 0;
                error_ = 0;
                pio_mode_ = PioMode::kNone;
                xfer_ = Xfer::kNone;
                xfer_active_ = false;
                long_ = false;
            }
            srst_prev_ = srst_now;
            nien_ = (v & 0x02) != 0;
            break;
        }
        default: break;
    }
}

void Wd1003::complete(uint8_t error) {
    xfer_ = Xfer::kNone;
    xfer_active_ = false;
    error_ = error;
    status_ = uint8_t(ST_DRDY | ST_DSC | (error ? ST_ERR : 0));
    irq_pending_ = true;
}

// Command codes per WD1003-WA2 OEM manual Table 5-1.
void Wd1003::run_command(uint8_t cmd) {
    error_ = 0;
    switch (cmd & 0xF0) {
        case 0x10: complete(); return;  // RESTORE
        case 0x70: complete(); return;  // SEEK
        default: break;
    }
    switch (cmd) {
        case 0xEC: do_identify(); break;  // ATA, not WD1003: the stand-in BIOS's ata_detect() needs it
        case 0x91: complete(); break;     // SET PARAMETERS: accepted, geometry is fixed
        case 0x90: do_diagnose(); break;
        case 0x20: case 0x21: case 0x22: case 0x23: begin_read(Xfer::kRead, cmd); break;
        case 0x30: case 0x31: case 0x32: case 0x33: begin_write(cmd); break;
        case 0x40: case 0x41: begin_read(Xfer::kVerify, cmd); break;
        case 0x50: begin_format(); break;
        default: complete(ERR_ABRT); break;
    }
}

// Result code 01 is "no errors" (WD1003-WA2 OEM manual 5.2.7).
void Wd1003::do_diagnose() {
    precomp_ = 32;
    sector_count_ = 1;
    cyl_low_ = cyl_high_ = 0;
    drive_head_ = 0;
    selected_drive_ = 0;
    complete();
    error_ = 0x01;
}

long Wd1003::first_bad(long first, int count) const {
    const Drive &d = drives[selected_drive_];
    if (d.bad_sectors.empty()) return -1;
    auto it = d.bad_sectors.lower_bound(first);
    return (it != d.bad_sectors.end() && *it < first + count) ? *it : -1;
}

void Wd1003::do_identify() {
    Drive &d = drives[selected_drive_];
    pio_buffer_.assign(512, 0);

    auto put16 = [&](int word_idx, uint16_t v) {
        pio_buffer_[std::size_t(word_idx) * 2] = uint8_t(v & 0xFF);
        pio_buffer_[std::size_t(word_idx) * 2 + 1] = uint8_t(v >> 8);
    };
    // ATA string fields swap character pairs within each 16-bit word.
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
    // Word 5 (bytes per sector) is obsolete, but the BIOS reads it from its cached
    // IDENTIFY copy to size its `rep insw` count (rombios.c ata_detect() ->
    // ata_cmd_data_io()). Left at zero, the BIOS drained nothing and reported
    // failure, which broke the FreeDOS installer's auto-partition step.
    put16(5, Drive::kBytesPerSector);
    put16(6, uint16_t(d.sectors_per_track));
    put_string(10, 10, "0");                  // serial number
    put_string(23, 4, "1.0");                  // firmware revision
    put_string(27, 20, "IBM WD1003 ST-4038");  // model number
    long total = d.capacity_sectors();
    put16(60, uint16_t(total & 0xFFFF));
    put16(61, uint16_t((total >> 16) & 0xFFFF));
    // Word 83 bit 10 (LBA48) stays 0; the BIOS falls back to words 60/61.

    pio_pos_ = 0;
    pio_mode_ = PioMode::kReadDrain;
    xfer_ = Xfer::kNone;
    status_ = ST_DRQ | ST_DRDY | ST_DSC;
    irq_pending_ = true;
}

long Wd1003::offset_for_current_registers() const {
    const Drive &d = drives[selected_drive_];
    // Drive/Head bit 6 selects 28-bit LBA: sector number = LBA[7:0], cylinder
    // low/high = LBA[15:8]/[23:16], head low nibble = LBA[27:24]. A real WD1003
    // has no LBA, but the BIOS (rombios.c ata_cmd_data_io()) always issues LBA.
    // CHS-only broke the FreeDOS auto-partition step (IBM_PCAT_REVIEW.md).
    if (drive_head_ & 0x40) {
        uint32_t lba = uint32_t(sector_number_) | (uint32_t(cyl_low_) << 8) |
                       (uint32_t(cyl_high_) << 16) | (uint32_t(drive_head_ & 0x0F) << 24);
        return long(lba) * Drive::kBytesPerSector;
    }
    int cyl = int(cyl_low_) | (int(cyl_high_) << 8);
    int head = drive_head_ & 0x0F;
    return d.offset_for(cyl, head, sector_number_);
}

void Wd1003::begin_read(Xfer kind, uint8_t cmd) {
    xfer_ = kind;
    long_ = (cmd & 0x02) != 0;
    no_retry_ = (cmd & 0x01) != 0;
    corrected_ = false;
    sectors_left_ = sector_count_ == 0 ? 256 : sector_count_;
    xfer_offset_ = offset_for_current_registers();
    seek_sector();
}

double Wd1003::sector_due(long sector) const {
    const Drive &d = drives[selected_drive_];
    long track = sector / d.sectors_per_track;
    int r = int(sector % d.sectors_per_track);
    int slot = (r * kFactoryInterleave) % d.sectors_per_track, slots = d.sectors_per_track;
    auto it = d.layout.find(track);
    if (it != d.layout.end()) {
        slot = it->second.first[std::size_t(r)];
        slots = it->second.second;
    }
    double now = double(prev_cycles_);
    double slot_len = kCyclesPerRev / slots;
    double wait = slot * slot_len - std::fmod(now, kCyclesPerRev);
    if (wait < 0) wait += kCyclesPerRev;
    return now + wait + slot_len;
}

// An ID that never passes the head is searched for 10 revolutions, 2 with T set (WD1003-WA2 OEM manual 5.2.3).
void Wd1003::seek_sector() {
    const Drive &d = drives[selected_drive_];
    long sector = xfer_offset_ / Drive::kBytesPerSector;
    id_missing_ = xfer_offset_ < 0 || std::size_t(xfer_offset_) + Drive::kBytesPerSector > d.image.size();
    if (!id_missing_) {
        auto it = d.layout.find(sector / d.sectors_per_track);
        id_missing_ = it != d.layout.end() && it->second.first[std::size_t(sector % d.sectors_per_track)] < 0;
    }
    due_ = id_missing_ ? double(prev_cycles_) + (no_retry_ ? 2 : 10) * kCyclesPerRev : sector_due(sector);
    retries_ = 0;
    xfer_active_ = true;
    status_ = ST_BSY;
}

// READ interrupts per sector as it reaches the buffer, none at the end (WD1003-WA2 OEM manual 5.2.3).
// An ECC error is reread up to eight times; a correctable one is fixed on the second matching syndrome.
void Wd1003::sector_arrived() {
    Drive &d = drives[selected_drive_];
    if (id_missing_) {
        complete(ERR_IDNF);
        return;
    }
    long sector = xfer_offset_ / Drive::kBytesPerSector;
    if (first_bad(sector, 1) >= 0) {
        complete(ERR_BB);
        return;
    }
    auto src = d.image.begin() + xfer_offset_;
    std::vector<uint8_t> data(src, src + Drive::kBytesPerSector);
    uint32_t computed = data_ecc(data.data());
    auto ov = d.ecc_override.find(sector);
    uint32_t stored = ov != d.ecc_override.end() ? ov->second : computed;
    bool uncorrectable = false;
    if (!long_ && stored != computed) {
        const uint8_t ecc[4] = {uint8_t(stored >> 24), uint8_t(stored >> 16), uint8_t(stored >> 8), uint8_t(stored)};
        uint32_t syndrome = ecc_shift(computed, ecc, 4);
        std::vector<uint8_t> fixed = data;
        bool correctable = correct_burst(syndrome, fixed.data());
        if (retries_ < (correctable ? 1 : 8)) {
            ++retries_;
            due_ += kCyclesPerRev;
            return;
        }
        if (correctable) {
            data = fixed;
            corrected_ = true;
        } else {
            uncorrectable = true;
        }
    }
    xfer_active_ = false;
    xfer_offset_ += Drive::kBytesPerSector;
    --sectors_left_;
    if (xfer_ == Xfer::kVerify) {
        if (uncorrectable) complete(ERR_ECC);
        else if (sectors_left_ > 0) seek_sector();
        else {
            complete();
            if (corrected_) status_ |= ST_CORR;
        }
        return;
    }
    if (long_) {
        for (int i = 3; i >= 0; --i) data.push_back(uint8_t(stored >> (8 * i)));
    }
    pio_buffer_ = std::move(data);
    pio_pos_ = 0;
    pio_mode_ = PioMode::kReadDrain;
    error_ = uncorrectable ? ERR_ECC : 0;
    status_ = uint8_t(ST_DRQ | ST_DRDY | ST_DSC | (uncorrectable ? ST_ERR : 0) | (corrected_ ? ST_CORR : 0));
    corrected_ = false;
    irq_pending_ = true;
}

void Wd1003::begin_write(uint8_t cmd) {
    long_ = (cmd & 0x02) != 0;
    int count = sector_count_ == 0 ? 256 : sector_count_;
    xfer_len_ = std::size_t(count) * (Drive::kBytesPerSector + (long_ ? 4 : 0));
    xfer_offset_ = offset_for_current_registers();
    xfer_ = Xfer::kWrite;
    pio_buffer_.assign(xfer_len_, 0);
    pio_pos_ = 0;
    pio_mode_ = PioMode::kWriteFill;
    status_ = ST_DRQ | ST_DRDY;
    xfer_active_ = false;
}

void Wd1003::finish_write() {
    Drive &d = drives[selected_drive_];
    std::size_t record = Drive::kBytesPerSector + (long_ ? 4 : 0);
    int count = int(xfer_len_ / record);
    if (xfer_offset_ < 0 || std::size_t(xfer_offset_) + std::size_t(count) * Drive::kBytesPerSector > d.image.size()) {
        complete(ERR_IDNF);
        return;
    }
    long first = xfer_offset_ / Drive::kBytesPerSector;
    long bad = first_bad(first, count);
    int good = bad < 0 ? count : int(bad - first);
    for (int i = 0; i < good; ++i) {
        const uint8_t* src = pio_buffer_.data() + std::size_t(i) * record;
        std::copy(src, src + Drive::kBytesPerSector, d.image.begin() + xfer_offset_ + long(i) * Drive::kBytesPerSector);
        d.ecc_override.erase(first + i);
        if (long_) {
            const uint8_t* e = src + Drive::kBytesPerSector;
            uint32_t ecc = uint32_t(e[0]) << 24 | uint32_t(e[1]) << 16 | uint32_t(e[2]) << 8 | e[3];
            if (ecc != data_ecc(src)) d.ecc_override[first + i] = ecc;
        }
    }
    if (good > 0) d.dirty = true;
    complete(bad < 0 ? 0 : ERR_BB);
}

// The host sends a 512-byte buffer of (flag, sector) pairs; 80h marks a bad block (WD1003-WA2 OEM manual 5.2.5).
void Wd1003::begin_format() {
    xfer_ = Xfer::kFormat;
    pio_buffer_.assign(Drive::kBytesPerSector, 0);
    pio_pos_ = 0;
    pio_mode_ = PioMode::kWriteFill;
    status_ = ST_DRQ | ST_DRDY;
    xfer_active_ = false;
}

// Data fields end up zeroed (WD1003-WA2 OEM manual 5.2.5). No error reporting.
void Wd1003::finish_format() {
    Drive &d = drives[selected_drive_];
    int cyl = int(cyl_low_) | (int(cyl_high_) << 8);
    int head = drive_head_ & 0x0F;
    if (d.present && cyl < d.cylinders && head < d.heads) {
        long track_index = long(cyl) * d.heads + head;
        long track = track_index * d.sectors_per_track;
        d.bad_sectors.erase(d.bad_sectors.lower_bound(track),
                            d.bad_sectors.lower_bound(track + d.sectors_per_track));
        d.ecc_override.erase(d.ecc_override.lower_bound(track),
                             d.ecc_override.lower_bound(track + d.sectors_per_track));
        int count = std::min(sector_count_ == 0 ? 256 : int(sector_count_), Drive::kBytesPerSector / 2);
        std::vector<int> slot_of(std::size_t(d.sectors_per_track), -1);
        for (int i = 0; i < count; ++i) {
            uint8_t flag = pio_buffer_[std::size_t(i) * 2];
            int r = pio_buffer_[std::size_t(i) * 2 + 1];
            if (r < 1 || r > d.sectors_per_track) continue;
            slot_of[std::size_t(r - 1)] = i;
            long off = d.offset_for(cyl, head, r);
            std::fill(d.image.begin() + off, d.image.begin() + off + Drive::kBytesPerSector, uint8_t(0));
            if (flag & 0x80) d.bad_sectors.insert(track + r - 1);
        }
        d.layout[track_index] = {slot_of, std::max(count, 1)};
        d.dirty = true;
    }
    complete();
}

void Wd1003::tick(uint64_t cpu_cycles) {
    prev_cycles_ = cpu_cycles;
    if (!xfer_active_ || double(cpu_cycles) < due_) return;
    switch (xfer_) {
        case Xfer::kRead: case Xfer::kVerify: sector_arrived(); break;
        case Xfer::kFormat: finish_format(); break;
        default: xfer_active_ = false; break;
    }
}

}  // namespace ibmpcat
