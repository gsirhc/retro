#include "atapi_cdrom.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

namespace pc486 {

namespace {

// SCSI sense keys (SPC §7.20.4, Table 69).
constexpr uint8_t kKeyNoSense = 0x00;
constexpr uint8_t kKeyNotReady = 0x02;
constexpr uint8_t kKeyIllegalRequest = 0x05;
constexpr uint8_t kKeyUnitAttention = 0x06;
constexpr uint8_t kKeyAbortedCommand = 0x0B;

// Additional sense codes (SPC Annex D / MMC Table 71).
constexpr uint8_t kAscInvalidOpcode = 0x20;        // INVALID COMMAND OPERATION CODE
constexpr uint8_t kAscLbaOutOfRange = 0x21;        // LOGICAL BLOCK ADDRESS OUT OF RANGE
constexpr uint8_t kAscInvalidFieldInCdb = 0x24;
constexpr uint8_t kAscMediumChanged = 0x28;        // NOT READY TO READY CHANGE
constexpr uint8_t kAscResetOccurred = 0x29;        // POWER ON, RESET, OR BUS DEVICE RESET
constexpr uint8_t kAscMediumNotPresent = 0x3A;
constexpr uint8_t kAscRemovalPrevented = 0x53;     // MEDIA REMOVAL PREVENTED (ASCQ 02h)
// MMC Table 73/77's PLAY AUDIO-specific codes.
constexpr uint8_t kAscEndOfUserArea = 0x63;        // END OF USER AREA ENCOUNTERED ON THIS TRACK
constexpr uint8_t kAscIllegalModeForTrack = 0x64;  // ILLEGAL MODE FOR THIS TRACK OR INCOMPATIBLE MEDIUM
constexpr uint8_t kAscPlayOperationAborted = 0xB9; // PLAY OPERATION ABORTED (sense key 0Bh)

// CDB fields are big-endian (SPC §3.4.2).
uint16_t be16(const uint8_t *p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }
uint32_t be32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void put_be16(uint8_t *p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v & 0xFF); }
void put_be32(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

// Space-padded, not NUL-terminated (SCSI INQUIRY / ATA IDENTIFY strings).
void put_padded(uint8_t *dst, std::size_t width, const char *s) {
    std::size_t len = std::strlen(s);
    for (std::size_t i = 0; i < width; ++i) dst[i] = uint8_t(i < len ? s[i] : ' ');
}

}  // namespace

void AtapiCdrom::reset() {
    assert_signature();
    // DRDY stays clear after reset (ATA/ATAPI-4 §9.1).
    status_ = 0x00;
    error_ = 0x01;  // "device passed diagnostics", the post-reset error code
    features_ = 0;
    drive_head_ = 0xA0;
    selected_device_ = 0;
    nien_ = false;
    srst_prev_ = false;
    irq_pending_ = false;
    phase_ = Phase::kIdle;
    cdb_pos_ = 0;
    limit_ = 0;
    data_.clear();
    data_pos_ = 0;
    block_end_ = 0;
    sense_key_ = kKeyNoSense;
    asc_ = ascq_ = 0;
    check_pending_ = false;
    locked_ = false;
    exec_active_ = false;
    exec_credit_ = exec_target_ = 0.0;
    prev_cycles_ = 0;
    head_lba_ = kHeadUnknown;
    playing_audio_ = false;
    audio_paused_ = false;
    audio_status_ = 0x15;  // MMC Table 116: no current audio status
    audio_credit_ = 0.0;
    audio_samples_.clear();
    // Mode page 0Eh defaults (SFF-8020i Table 60): ports 0/1 straight through at
    // full volume, 2/3 muted.
    audio_ports_[0] = AudioPort{0x1, 0xFF};
    audio_ports_[1] = AudioPort{0x2, 0xFF};
    audio_ports_[2] = AudioPort{0x0, 0x00};
    audio_ports_[3] = AudioPort{0x0, 0x00};
    // Reset raises unit attention (SPC §5.6).
    set_unit_attention(kAscResetOccurred, 0x00);
    // image_/media_present_ survive: a reset does not eject the disc.
}

void AtapiCdrom::assert_signature() {
    // ATA/ATAPI-4 §9.1: packet devices report 01h/01h/14h/EBh, ATA disks 01h/01h/00h/00h.
    int_reason_ = 0x01;
    sector_number_ = 0x01;
    byte_count_ = 0xEB14;
}

void AtapiCdrom::mount(const uint8_t *data, std::size_t len) {
    // Whole 2048-byte sectors only.
    len -= len % kBytesPerSector;
    image_.assign(data, data + len);
    audio_pcm_.clear();
    const uint32_t blocks = uint32_t(image_.size() / kBytesPerSector);
    tracks_.assign(1, Track{1, /*is_audio=*/false, 0, blocks, 0});
    media_present_ = len > 0;
    head_lba_ = kHeadUnknown;
    stop_audio_playback(0x15);
    set_unit_attention(kAscMediumChanged, 0x00);
}

bool AtapiCdrom::mount_cue(const char *cue_text, const uint8_t *bin_data, std::size_t bin_len) {
    image_.clear();
    audio_pcm_.clear();
    tracks_.clear();
    if (!parse_cue(cue_text, bin_len)) {
        tracks_.clear();
        media_present_ = false;
        return false;
    }
    // Single-FILE CUE tracks are contiguous in the BIN, so walk a byte cursor
    // (sector size differs by track type). MODE1 goes to image_, AUDIO to
    // audio_pcm_ at 588 frames/LBA.
    std::size_t bin_cursor = 0;
    for (auto &t : tracks_) {
        const std::size_t sector_bytes = t.is_audio ? 2352 : std::size_t(kBytesPerSector);
        const std::size_t len = std::size_t(t.length_lba) * sector_bytes;
        if (bin_cursor + len > bin_len) { tracks_.clear(); media_present_ = false; return false; }
        if (t.is_audio) {
            t.pcm_base_frame = audio_pcm_.size() / 2;
            audio_pcm_.resize(audio_pcm_.size() + std::size_t(t.length_lba) * 588 * 2);
            int16_t *dst = audio_pcm_.data() + t.pcm_base_frame * 2;
            for (std::size_t i = 0; i < len / 2; ++i) {
                dst[i] = int16_t(uint16_t(bin_data[bin_cursor + i * 2]) |
                                  (uint16_t(bin_data[bin_cursor + i * 2 + 1]) << 8));
            }
        } else {
            image_.insert(image_.end(), bin_data + bin_cursor, bin_data + bin_cursor + len);
        }
        bin_cursor += len;
    }
    media_present_ = !tracks_.empty();
    head_lba_ = kHeadUnknown;
    stop_audio_playback(0x15);
    set_unit_attention(kAscMediumChanged, 0x00);
    return true;
}

void AtapiCdrom::eject() {
    // Host eject ignores the removal lock (emergency eject); the CDB path honors it.
    image_.clear();
    audio_pcm_.clear();
    tracks_.clear();
    media_present_ = false;
    head_lba_ = kHeadUnknown;
    stop_audio_playback(0x15);
    set_unit_attention(kAscMediumChanged, 0x00);
}

bool AtapiCdrom::owns(uint16_t port) const {
    return (port >= 0x170 && port <= 0x177) || port == 0x376;
}

uint32_t AtapiCdrom::capacity_blocks() const {
    return uint32_t(image_.size() / kBytesPerSector);
}

namespace {
// "MM:SS:FF" to frame count (75 frames/s).
bool parse_msf_field(const std::string &s, uint32_t *out) {
    if (s.size() != 8 || s[2] != ':' || s[5] != ':') return false;
    for (int i : {0, 1, 3, 4, 6, 7}) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    int mm = std::atoi(s.substr(0, 2).c_str());
    int ss = std::atoi(s.substr(3, 2).c_str());
    int ff = std::atoi(s.substr(6, 2).c_str());
    *out = uint32_t(mm) * 60 * 75 + uint32_t(ss) * 75 + uint32_t(ff);
    return true;
}
}  // namespace

// CUE subset: TRACK <n> <MODE1/2048|AUDIO>, INDEX <n> <MM:SS:FF>. Only INDEX 01
// is used; mount_cue() lays out the bytes.
bool AtapiCdrom::parse_cue(const char *cue_text, std::size_t bin_len) {
    struct Entry { int number; bool is_audio; uint32_t lba; };
    std::vector<Entry> entries;
    int pend_number = 0;
    bool pend_audio = false;
    bool have_pending = false;
    std::istringstream file(cue_text ? cue_text : "");
    std::string raw;
    while (std::getline(file, raw)) {
        std::size_t b = raw.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        std::istringstream line(raw.substr(b));
        std::string kw;
        line >> kw;
        for (char &c : kw) c = char(std::toupper(static_cast<unsigned char>(c)));
        if (kw == "TRACK") {
            std::string mode;
            if (!(line >> pend_number >> mode)) return false;
            for (char &c : mode) c = char(std::toupper(static_cast<unsigned char>(c)));
            pend_audio = (mode == "AUDIO");
            have_pending = true;
        } else if (kw == "INDEX" && have_pending) {
            int idx = 0;
            std::string ts;
            if (!(line >> idx >> ts)) return false;
            if (idx == 1) {
                uint32_t lba = 0;
                if (!parse_msf_field(ts, &lba)) return false;
                entries.push_back({pend_number, pend_audio, lba});
                have_pending = false;
            }
        }
        // FILE, REM, CATALOG, PREGAP etc. are ignored.
    }
    if (entries.empty()) return false;
    tracks_.clear();
    tracks_.reserve(entries.size());
    // The last track's length comes from the remaining BIN bytes, summed in each
    // track's own sector size.
    std::size_t bytes_so_far = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        Track t;
        t.number = entries[i].number;
        t.is_audio = entries[i].is_audio;
        t.start_lba = entries[i].lba;
        const std::size_t sector_bytes = t.is_audio ? 2352u : std::size_t(kBytesPerSector);
        if (i + 1 < entries.size()) {
            const uint32_t end_lba = entries[i + 1].lba;
            if (end_lba <= t.start_lba) return false;
            t.length_lba = end_lba - t.start_lba;
        } else {
            if (bin_len <= bytes_so_far) return false;
            const uint32_t len_lba = uint32_t((bin_len - bytes_so_far) / sector_bytes);
            if (len_lba == 0) return false;
            t.length_lba = len_lba;
        }
        bytes_so_far += std::size_t(t.length_lba) * sector_bytes;
        tracks_.push_back(t);
    }
    return true;
}

const AtapiCdrom::Track *AtapiCdrom::track_at(uint32_t lba) const {
    for (const auto &t : tracks_) {
        if (lba >= t.start_lba && lba < t.start_lba + t.length_lba) return &t;
    }
    return nullptr;
}

const AtapiCdrom::Track *AtapiCdrom::track_number(int n) const {
    for (const auto &t : tracks_) {
        if (t.number == n) return &t;
    }
    return nullptr;
}

// --- register interface ---------------------------------------------------

uint8_t AtapiCdrom::in(uint16_t port) {
    if (selected_device_absent()) {
        // ATA/ATAPI-6 Table 18: device 0 responding for absent device 1.
        if (port == 0x171) return error_;
        if (port == 0x170) return 0xFF;
        return 0x00;
    }
    switch (port) {
        case 0x170: return read_data_byte();
        case 0x171: return error_;
        case 0x172: return int_reason_;
        case 0x173: return sector_number_;
        case 0x174: return uint8_t(byte_count_ & 0xFF);
        case 0x175: return uint8_t(byte_count_ >> 8);
        case 0x176: return drive_head_;
        case 0x177: irq_pending_ = false; return status_;  // reading status acknowledges the interrupt
        case 0x376: return status_;  // alternate status: same bits, does NOT acknowledge
        default: return 0xFF;
    }
}

void AtapiCdrom::out(uint16_t port, uint8_t v) {
    // Writes land in device 0 even with device 1 selected; only EXECUTE DEVICE
    // DIAGNOSTIC is acted on (ATA/ATAPI-6 Table 18).
    if (port == 0x177 && selected_device_absent() && v != 0x90) return;
    switch (port) {
        case 0x170: write_data_byte(v); break;
        case 0x171: features_ = v; break;
        // Shared latches, see atapi_cdrom.h.
        case 0x172: int_reason_ = v; break;
        case 0x173: sector_number_ = v; break;
        case 0x174: byte_count_ = uint16_t((byte_count_ & 0xFF00) | v); break;
        case 0x175: byte_count_ = uint16_t((byte_count_ & 0x00FF) | (uint16_t(v) << 8)); break;
        case 0x176: drive_head_ = v; selected_device_ = (v & 0x10) ? 1 : 0; break;
        case 0x177: run_command(v); break;
        case 0x376: {
            bool srst_now = (v & 0x04) != 0;
            if (srst_now && !srst_prev_) {
                // SRST 1->0 edge reasserts the signature regardless of DEV (ATA/ATAPI-6 Table 18).
                assert_signature();
                status_ = 0x00;
                error_ = 0x01;
                phase_ = Phase::kIdle;
                cdb_pos_ = 0;
                data_.clear();
                data_pos_ = 0;
                block_end_ = 0;
                exec_active_ = false;
                check_pending_ = false;
                stop_audio_playback(0x15);
                set_unit_attention(kAscResetOccurred, 0x00);
            }
            srst_prev_ = srst_now;
            nien_ = (v & 0x02) != 0;
            break;
        }
        default: break;
    }
}

uint16_t AtapiCdrom::data_in16() {
    uint8_t lo = read_data_byte();
    uint8_t hi = read_data_byte();
    return uint16_t(uint16_t(lo) | (uint16_t(hi) << 8));
}

void AtapiCdrom::data_out16(uint16_t v) {
    write_data_byte(uint8_t(v & 0xFF));
    write_data_byte(uint8_t(v >> 8));
}

// --- ATA-level command layer ---------------------------------------------

void AtapiCdrom::run_command(uint8_t cmd) {
    switch (cmd) {
        case 0xA0: begin_packet(); break;             // PACKET
        case 0xA1: identify_packet_device(); break;   // IDENTIFY PACKET DEVICE
        case 0x08: device_reset(); break;             // DEVICE RESET (packet devices only)
        case 0xEC:
            // IDENTIFY DEVICE aborts with the ATAPI signature (ATA/ATAPI-4 §8.12.1).
            abort_command(/*with_signature=*/true);
            break;
        case 0x90:
            // Pass and reassert the signature (ATA/ATAPI-4 §8.9).
            assert_signature();
            error_ = 0x01;
            status_ = ST_DRDY;
            irq_pending_ = !nien_;
            break;
        case 0xEF:
            // SET FEATURES: accepted, nothing to change (PIO only).
            error_ = 0;
            status_ = ST_DRDY;
            irq_pending_ = !nien_;
            break;
        default:
            abort_command(/*with_signature=*/false);
            break;
    }
}

void AtapiCdrom::abort_command(bool with_signature) {
    if (with_signature) assert_signature();
    phase_ = Phase::kIdle;
    data_.clear();
    data_pos_ = block_end_ = 0;
    exec_active_ = false;
    // Sense key in 7:4, ABRT in the low nibble (ATA/ATAPI-4 §7.6.1).
    sense_key_ = kKeyIllegalRequest;
    asc_ = kAscInvalidOpcode;
    ascq_ = 0;
    check_pending_ = true;
    error_ = uint8_t((kKeyIllegalRequest << 4) | 0x04);
    status_ = ST_DRDY | ST_CHK;
    irq_pending_ = !nien_;
}

void AtapiCdrom::device_reset() {
    // ATA/ATAPI-4 §8.7: resets protocol state, reasserts the signature, no INTRQ.
    assert_signature();
    status_ = 0x00;
    error_ = 0x01;
    phase_ = Phase::kIdle;
    cdb_pos_ = 0;
    data_.clear();
    data_pos_ = block_end_ = 0;
    exec_active_ = false;
    check_pending_ = false;
    stop_audio_playback(0x15);
    set_unit_attention(kAscResetOccurred, 0x00);
}

void AtapiCdrom::identify_packet_device() {
    data_.assign(512, 0);
    auto put16 = [&](int word_idx, uint16_t v) {
        data_[std::size_t(word_idx) * 2] = uint8_t(v & 0xFF);
        data_[std::size_t(word_idx) * 2 + 1] = uint8_t(v >> 8);
    };
    // ATA strings are byte-swapped within each word (ATA/ATAPI-4 §8.12.8).
    auto put_string = [&](int word_start, int word_count, const char *s) {
        std::size_t len = std::strlen(s);
        for (int i = 0; i < word_count * 2; ++i) {
            char c = (std::size_t(i) < len) ? s[i] : ' ';
            std::size_t byte_idx = std::size_t(word_start) * 2 + std::size_t(i ^ 1);
            if (byte_idx < data_.size()) data_[byte_idx] = uint8_t(c);
        }
    };

    // Word 0 (ATA/ATAPI-4 §8.13.8 Table 12): ATAPI, CD-ROM command set (05h),
    // removable, accelerated DRQ (bits 6:5 = 10b), 12-byte packet.
    put16(0, 0x85C0);
    put_string(10, 10, "RW-CD-0001");         // serial number, words 10-19
    put_string(23, 4, "1.0");                 // firmware revision, words 23-26
    put_string(27, 20, "RETROWEB CD-ROM 2X"); // model number, words 27-46
    // Word 49 (§8.13.8): LBA (bit 9) and IORDY (bit 11). No DMA bit.
    put16(49, 0x0A00);
    put16(51, 0x0200);  // PIO cycle timing mode 2 in the high byte (legacy field)
    put16(53, 0x0002);  // field validity: words 64-70 valid (words 54-58 are CHS, meaningless for ATAPI)
    put16(63, 0x0000);  // multiword DMA modes: none supported
    put16(64, 0x0003);  // advanced PIO modes 3 and 4 supported
    put16(67, 240);     // minimum PIO cycle time without flow control, ns
    put16(68, 120);     // minimum PIO cycle time with IORDY, ns
    put16(71, 30);      // time from PACKET receipt to bus release, us (overlap not used)
    put16(72, 30);      // time from SERVICE to nBSY, us
    put16(80, 0x0010);  // major version: ATA/ATAPI-4
    put16(82, 0x0210);  // command sets supported: PACKET (bit 4) + DEVICE RESET (bit 9)
    put16(83, 0x4000);  // bit 14 set = words 82-84 are valid
    put16(84, 0x4000);
    put16(85, 0x0210);  // ...and the same sets are enabled
    put16(86, 0x0000);
    put16(87, 0x4000);

    // Plain PIO data-in, so the byte-count limit doesn't apply.
    data_pos_ = 0;
    block_end_ = data_.size();
    byte_count_ = uint16_t(data_.size());
    int_reason_ = IR_IO;
    phase_ = Phase::kDataIn;
    check_pending_ = false;
    error_ = 0;
    status_ = ST_DRDY | ST_DRQ;
    irq_pending_ = !nien_;
}

// --- packet protocol ------------------------------------------------------

void AtapiCdrom::begin_packet() {
    if (features_ & 0x01) {
        // DMA is unsupported; abort so the driver falls back to PIO (ATA/ATAPI-4 §9.6).
        abort_command(/*with_signature=*/false);
        return;
    }
    // ATA/ATAPI-4 §7.3.2: an odd limit rounds down to even (0xFFFF to 0xFFFE).
    // Zero means no limit.
    limit_ = byte_count_ & 0xFFFE;
    if (limit_ == 0) limit_ = 0xFFFE;

    cdb_pos_ = 0;
    std::memset(cdb_, 0, sizeof(cdb_));
    data_.clear();
    data_pos_ = block_end_ = 0;
    check_pending_ = false;
    error_ = 0;
    // C/D=1, I/O=0, no INTRQ: accelerated-DRQ device (ATA/ATAPI-4 §9.6.1).
    int_reason_ = IR_CD;
    status_ = ST_DRDY | ST_DRQ;
    phase_ = Phase::kCommandPacket;
}

void AtapiCdrom::write_data_byte(uint8_t v) {
    if (phase_ == Phase::kDataOut) {
        // MODE SELECT(10) is the only data-out CDB.
        if (data_pos_ < data_.size()) data_[data_pos_++] = v;
        if (data_pos_ >= data_.size()) {
            status_ = ST_BSY;
            phase_ = Phase::kExecuting;
            mode_select_apply();
        }
        return;
    }
    if (phase_ != Phase::kCommandPacket) return;  // no other data-out CDBs: see the file header
    if (cdb_pos_ < int(sizeof(cdb_))) cdb_[cdb_pos_++] = v;
    if (cdb_pos_ >= int(sizeof(cdb_))) {
        // Full 12-byte packet in hand: BSY up, DRQ down, then execute.
        status_ = ST_BSY;
        phase_ = Phase::kExecuting;
        execute_packet();
    }
}

uint8_t AtapiCdrom::read_data_byte() {
    if (phase_ != Phase::kDataIn || data_pos_ >= block_end_) return 0xFF;
    uint8_t b = data_[data_pos_++];
    if (data_pos_ >= block_end_) {
        if (data_pos_ < data_.size()) start_data_in_block();
        else command_complete();
    }
    return b;
}

void AtapiCdrom::begin_execute(double seconds) {
    exec_active_ = true;
    exec_credit_ = 0.0;
    exec_target_ = seconds * cpu_hz_;
    status_ = ST_BSY;
    if (exec_target_ <= 0.0) finish_execute();
}

void AtapiCdrom::set_cpu_hz(double hz) {
    if (!(hz > 0.0) || hz == cpu_hz_) return;
    const double scale = hz / cpu_hz_;
    if (exec_active_) exec_target_ = exec_credit_ + (exec_target_ - exec_credit_) * scale;
    cpu_hz_ = hz;
}

void AtapiCdrom::finish_execute() {
    exec_active_ = false;
    if (check_pending_ || data_.empty()) {
        command_complete();
        return;
    }
    data_pos_ = 0;
    block_end_ = 0;
    start_data_in_block();
}

void AtapiCdrom::start_data_in_block() {
    std::size_t remaining = data_.size() - data_pos_;
    std::size_t block = std::min(remaining, std::size_t(limit_));
    block_end_ = data_pos_ + block;
    // Byte count tells the host how many words to read (ATA/ATAPI-4 §9.6.2).
    byte_count_ = uint16_t(block);
    int_reason_ = IR_IO;  // I/O = 1, C/D = 0: data to the host
    status_ = ST_DRDY | ST_DRQ;
    phase_ = Phase::kDataIn;
    irq_pending_ = !nien_;
}

void AtapiCdrom::command_complete() {
    phase_ = Phase::kIdle;
    data_.clear();
    data_pos_ = block_end_ = 0;
    byte_count_ = 0;
    // Completion sets I/O and C/D, clears DRQ: Interrupt Reason reads 03h (§9.6).
    int_reason_ = IR_CD | IR_IO;
    status_ = uint8_t(ST_DRDY | (check_pending_ ? ST_CHK : 0));
    irq_pending_ = !nien_;
}

void AtapiCdrom::set_check(uint8_t key, uint8_t asc, uint8_t ascq, bool abrt) {
    sense_key_ = key;
    asc_ = asc;
    ascq_ = ascq;
    check_pending_ = true;
    data_.clear();
    error_ = uint8_t((key << 4) | (abrt ? 0x04 : 0x00));
}

void AtapiCdrom::set_unit_attention(uint8_t asc, uint8_t ascq) {
    ua_pending_ = true;
    ua_asc_ = asc;
    ua_ascq_ = ascq;
}

bool AtapiCdrom::require_media() {
    if (media_present_) return true;
    set_check(kKeyNotReady, kAscMediumNotPresent, 0x00);
    return false;
}

void AtapiCdrom::respond(const uint8_t *bytes, std::size_t len, std::size_t alloc) {
    len = std::min(len, alloc);
    data_.assign(bytes, bytes + len);
    // Odd lengths pad to a whole word (ATA/ATAPI-4 §9.6.2).
    if (data_.size() & 1) data_.push_back(0);
}

void AtapiCdrom::execute_packet() {
    const uint8_t op = cdb_[0];
    double seconds = kCommandSec;
    check_pending_ = false;
    error_ = 0;
    data_.clear();
    data_pos_ = block_end_ = 0;

    // SPC §5.6: pending unit attention gives CHECK CONDITION, except for INQUIRY
    // and REQUEST SENSE.
    if (ua_pending_ && op != 0x12 && op != 0x03) {
        ua_pending_ = false;
        set_check(kKeyUnitAttention, ua_asc_, ua_ascq_);
        begin_execute(seconds);
        return;
    }

    // MMC Table 74/75: only these opcodes stop a play operation.
    if (playing_audio_) {
        switch (op) {
            case 0x1B: case 0x28: case 0x2B:  // START/STOP UNIT, READ(10), SEEK(10)
                stop_audio_playback(0x15);
                break;
            default: break;
        }
    }

    switch (op) {
        case 0x00:  // TEST UNIT READY (SPC §7.25) -- status only, no data
            require_media();
            break;
        case 0x03: cmd_request_sense(); break;                        // SPC §7.20
        case 0x12: cmd_inquiry(); break;                              // SPC §7.5
        case 0x1B: cmd_start_stop_unit(); break;                      // MMC §6.1.13
        case 0x1E:                                                    // PREVENT ALLOW MEDIUM REMOVAL, SPC §7.12
            locked_ = (cdb_[4] & 0x01) != 0;
            break;
        case 0x25: if (require_media()) cmd_read_capacity(); break;    // MMC §6.1.10
        case 0x28: if (require_media()) seconds += cmd_read10(); break;  // MMC §6.1.7
        case 0x2B: if (require_media()) seconds += cmd_seek10(); break;  // MMC §6.1.15
        case 0x42: if (require_media()) cmd_read_subchannel(); break;  // MMC §6.1.9 / §10.8.18
        case 0x43: if (require_media()) cmd_read_toc(); break;         // MMC §6.1.12
        case 0x45: if (require_media()) cmd_play_audio10(); break;     // MMC §10.8.8
        case 0x47: if (require_media()) cmd_play_audio_msf(); break;   // MMC §10.8.9
        case 0x4B: cmd_pause_resume(); break;                          // MMC §10.8.7
        case 0x4E: cmd_stop_play_scan(); break;                        // MMC §10.8.24
        case 0x55: cmd_mode_select10(); return;                        // Data-OUT -- see below
        case 0x5A: cmd_mode_sense10(); break;                          // MMC §6.1.6 / SPC §7.10
        default:
            // MODE SENSE(6)/MODE SELECT(6) don't exist for ATAPI CD-ROM (SFF-8020i).
            set_check(kKeyIllegalRequest, kAscInvalidOpcode, 0x00, /*abrt=*/true);
            break;
    }
    begin_execute(seconds);
}

void AtapiCdrom::cmd_request_sense() {
    // SPC §7.20.2 fixed-format sense. Never fails for no media, since drivers use
    // it to learn why a command failed.
    uint8_t s[18] = {};
    s[0] = 0x70;               // current error, information fields not valid
    s[2] = uint8_t(sense_key_ & 0x0F);
    s[7] = 10;                 // additional sense length -> 18 bytes total
    s[12] = asc_;
    s[13] = ascq_;
    respond(s, sizeof(s), cdb_[4]);
    // Reading sense clears it (SPC §7.20).
    sense_key_ = kKeyNoSense;
    asc_ = ascq_ = 0;
}

void AtapiCdrom::cmd_inquiry() {
    // SPC §7.5.2 INQUIRY. Works with no disc; it describes the drive.
    uint8_t s[36] = {};
    s[0] = 0x05;  // peripheral device type: CD-ROM (MMC device)
    s[1] = 0x80;  // RMB: removable medium
    s[2] = 0x00;  // ANSI version: 0, what period ATAPI drives report
    s[3] = 0x21;  // ATAPI response data format (SFF-8020i)
    s[4] = 31;    // additional length -> 36 bytes total
    put_padded(s + 8, 8, "RETROWEB");
    put_padded(s + 16, 16, "CD-ROM 2X");
    put_padded(s + 32, 4, "1.0");
    respond(s, sizeof(s), cdb_[4]);
}

void AtapiCdrom::cmd_start_stop_unit() {
    const bool start = (cdb_[4] & 0x01) != 0;
    const bool load_eject = (cdb_[4] & 0x02) != 0;
    if (!load_eject) return;  // plain spin up/down: nothing to model, succeeds
    if (!start) {
        if (locked_) {
            // SPC §7.12: eject is refused while removal is prevented.
            set_check(kKeyIllegalRequest, kAscRemovalPrevented, 0x02);
            return;
        }
        if (media_present_) eject();
        return;
    }
    // LoEj+Start closes the tray; with no image it closes empty.
}

void AtapiCdrom::cmd_read_capacity() {
    // READ CAPACITY returns the last LBA, not the count.
    uint8_t s[8] = {};
    put_be32(s + 0, capacity_blocks() - 1);
    put_be32(s + 4, uint32_t(kBytesPerSector));
    respond(s, sizeof(s), sizeof(s));  // fixed-length response, no allocation-length field
}

double AtapiCdrom::cmd_read10() {
    const uint32_t lba = be32(&cdb_[2]);
    const uint32_t blocks = be16(&cdb_[7]);
    if (blocks == 0) return 0.0;  // a zero transfer length is not an error in SCSI
    const uint32_t cap = capacity_blocks();
    if (lba >= cap || uint64_t(lba) + blocks > cap) {
        set_check(kKeyIllegalRequest, kAscLbaOutOfRange, 0x00);
        return 0.0;
    }
    const std::size_t off = std::size_t(lba) * kBytesPerSector;
    const std::size_t len = std::size_t(blocks) * kBytesPerSector;
    data_.assign(image_.begin() + std::ptrdiff_t(off),
                 image_.begin() + std::ptrdiff_t(off + len));
    double seconds = double(len) / kBytesPerSec;
    seconds += access_seconds_for(lba);
    note_transfer(lba, blocks);
    return seconds;
}

double AtapiCdrom::cmd_seek10() {
    // MMC SEEK(10). FreeDOS ATAPICDD.SYS issues it for its Seek command.
    const uint32_t lba = be32(&cdb_[2]);
    if (lba >= capacity_blocks()) {
        set_check(kKeyIllegalRequest, kAscLbaOutOfRange, 0x00);
        return 0.0;
    }
    // Charges seek time for the distance; a following READ(10) of that LBA then pays none.
    double seconds = access_seconds_for(lba);
    note_transfer(lba, 0);
    return seconds;
}

// Access time given head position and read-ahead buffer (see atapi_cdrom.h).
double AtapiCdrom::access_seconds_for(uint32_t lba) const {
    // Cold: full average access.
    if (head_lba_ == kHeadUnknown) return kAccessSec;
    // Inside the read-ahead buffer: no head movement.
    const uint32_t start = head_lba_ > kBufferSectors ? head_lba_ - kBufferSectors : 0;
    if (lba >= start && lba <= head_lba_) return 0.0;
    const uint32_t cap = capacity_blocks();
    if (cap == 0) return kAccessSec;
    const uint32_t dist = lba > head_lba_ ? lba - head_lba_ : head_lba_ - lba;
    const double frac = double(dist) / double(cap);
    // Linear in disc fraction crossed, fitted so a one-third-stroke seek equals kAccessSec.
    return kSeekMinSec + (kAccessSec - kSeekMinSec) * 3.0 * frac;
}

// Head ends where the read-ahead buffer ends.
void AtapiCdrom::note_transfer(uint32_t lba, uint32_t blocks) {
    head_lba_ = lba + blocks;
}

// Lead-out address. capacity_blocks() covers only the data track.
uint32_t AtapiCdrom::disc_end_lba() const {
    return tracks_.empty() ? 0 : tracks_.back().start_lba + tracks_.back().length_lba;
}

void AtapiCdrom::cmd_read_toc() {
    // MMC READ TOC formats 0000b and 0001b. Drivers issue it for disc detection.
    const bool msf = (cdb_[1] & 0x02) != 0;
    const uint16_t alloc = be16(&cdb_[7]);
    const uint8_t format = uint8_t(cdb_[2] & 0x0F);
    if (format != 0x00 && format != 0x01) {
        set_check(kKeyIllegalRequest, kAscInvalidFieldInCdb, 0x00);
        return;
    }
    auto put_addr = [&](uint8_t *p, uint32_t lba) {
        if (!msf) { put_be32(p, lba); return; }
        // MSF form adds the 150-frame pregap (LBA 0 is 00:02:00).
        uint32_t f = lba + 150;
        p[0] = 0;
        p[1] = uint8_t(f / (75 * 60));
        p[2] = uint8_t((f / 75) % 60);
        p[3] = uint8_t(f % 75);
    };
    // ADR=1; CONTROL bit 2 set for data (0x14), clear for audio (0x10), MMC Table 118.
    auto control_for = [](bool is_audio) -> uint8_t { return is_audio ? 0x10 : 0x14; };
    if (format == 0x01) {
        // Session Information is 12 bytes. FreeDOS UDVD2.SYS issues 43 00 01 ... 00 0C
        // as its disc-present check.
        const Track first = tracks_.empty() ? Track{} : tracks_.front();
        uint8_t sess[12] = {};
        put_be16(sess + 0, 10);  // TOC data length: total minus this 2-byte field
        sess[2] = 1;             // first complete session
        sess[3] = 1;             // last complete session
        sess[4] = 0x00;          // reserved
        sess[5] = control_for(first.is_audio);
        sess[6] = uint8_t(first.number);  // first track number in the last complete session
        sess[7] = 0x00;                    // reserved
        put_addr(sess + 8, first.start_lba);
        respond(sess, sizeof(sess), alloc);
        return;
    }

    std::vector<uint8_t> s(4 + tracks_.size() * 8 + 8, 0);
    put_be16(s.data() + 0, uint16_t(s.size() - 2));
    s[2] = tracks_.empty() ? 0 : uint8_t(tracks_.front().number);
    s[3] = tracks_.empty() ? 0 : uint8_t(tracks_.back().number);
    uint8_t *p = s.data() + 4;
    for (const auto &t : tracks_) {
        p[0] = 0x00;
        p[1] = control_for(t.is_audio);
        p[2] = uint8_t(t.number);
        put_addr(p + 4, t.start_lba);
        p += 8;
    }
    p[0] = 0x00;
    p[1] = 0x14;
    p[2] = 0xAA;  // lead-out is reported as pseudo-track AAh
    put_addr(p + 4, disc_end_lba());
    respond(s.data(), s.size(), alloc);
}

// Page 0Eh (SFF-8020i Table 60): mirrors the last MODE SELECT so it round-trips.
void AtapiCdrom::append_audio_control_page(std::vector<uint8_t> *out) const {
    std::size_t base = out->size();
    out->resize(base + 16, 0);
    uint8_t *p = out->data() + base;
    p[0] = 0x0E;  // page code, PS = 0
    p[1] = 0x0E;  // page length: 14 more bytes
    p[2] = 0x04;  // Immed = 1 (mandatory); SOTC = 0 (play to transfer length, not track boundary)
    put_be16(p + 6, 75);  // logical blocks per second of audio playback -- Red Book's fixed rate
    for (int port = 0; port < 4; ++port) {
        p[8 + port * 2] = audio_ports_[port].channel_selection & 0x0F;
        p[9 + port * 2] = audio_ports_[port].volume;
    }
}

void AtapiCdrom::cmd_mode_sense10() {
    const uint8_t page = uint8_t(cdb_[2] & 0x3F);
    const uint16_t alloc = be16(&cdb_[7]);
    if (page != 0x0E && page != 0x2A && page != 0x3F) {
        set_check(kKeyIllegalRequest, kAscInvalidFieldInCdb, 0x00);
        return;
    }
    std::vector<uint8_t> s(8, 0);  // mode parameter header, 10-byte command form
    s[2] = media_present_ ? 0x01 : 0x70;  // medium type: 120mm data disc / door open
    // bytes 6-7 (block descriptor length) stay 0: no block descriptors.

    if (page == 0x0E || page == 0x3F) append_audio_control_page(&s);
    if (page == 0x2A || page == 0x3F) {
        // Page 2Ah, CD capabilities (SFF-8020i §10.8.6.4). Works with no disc.
        std::size_t base = s.size();
        s.resize(base + 20, 0);
        uint8_t *p = s.data() + base;
        p[0] = 0x2A;  // page code, PS = 0
        // Page length 12h: 20-byte page (SFF-8020i Table 68).
        p[1] = 0x12;
        p[2] = 0x00;  // no CD-R/CD-RW/DVD read capability
        p[3] = 0x00;  // no write capability
        // Byte 4 bit 0: Audio Play. Everything else clear.
        p[4] = 0x01;
        // Byte 5 bit 0 (CD-DA via READ CD) stays clear: no READ CD.
        p[5] = 0x00;
        // Loading mechanism type 001b (tray) in bits 7:5, Eject supported
        // (bit 3), Lock supported (bit 0).
        p[6] = 0x29;
        // Separate volume (bit 0) and channel mute (bit 1).
        p[7] = 0x03;
        put_be16(p + 8, 353);   // maximum read speed, KB/s: 2x = 2 x 176.4
        put_be16(p + 10, 256);  // number of volume levels: one per attenuation byte
        put_be16(p + 12, 256);  // buffer size, KB -- a period 2x drive's cache
        put_be16(p + 14, 353);  // current read speed
    }
    put_be16(s.data() + 0, uint16_t(s.size() - 2));  // mode data length
    respond(s.data(), s.size(), alloc);
}

void AtapiCdrom::cmd_mode_select10() {
    // SPC MODE SELECT(10), Parameter List Length at cdb_[7..8]. Page 0Eh only.
    const uint16_t param_len = be16(&cdb_[7]);
    if (param_len == 0) { command_complete(); return; }  // not an error, SPC §10.4
    data_.assign(param_len, 0);
    data_pos_ = 0;
    block_end_ = data_.size();
    byte_count_ = uint16_t(std::min<std::size_t>(data_.size(), limit_));
    int_reason_ = 0;  // C/D = 0, I/O = 0: "send me data", device to host direction clear
    status_ = ST_DRDY | ST_DRQ;
    phase_ = Phase::kDataOut;
    irq_pending_ = !nien_;
}

void AtapiCdrom::mode_select_apply() {
    // 8-byte header (no block descriptors), then the page.
    check_pending_ = false;
    error_ = 0;
    bool ok = data_.size() >= 8 + 16;
    if (ok) {
        const uint8_t *p = data_.data() + 8;
        ok = (p[0] & 0x3F) == 0x0E && p[1] >= 0x0E;
        if (ok) {
            for (int port = 0; port < 4; ++port) {
                audio_ports_[port].channel_selection = p[8 + port * 2] & 0x0F;
                audio_ports_[port].volume = p[9 + port * 2];
            }
        }
    }
    if (!ok) set_check(kKeyIllegalRequest, kAscInvalidFieldInCdb, 0x00);
    data_.clear();
    data_pos_ = block_end_ = 0;
    begin_execute(kCommandSec);
}

void AtapiCdrom::cmd_play_audio10() {
    uint32_t lba = be32(&cdb_[2]);
    const uint32_t len = be16(&cdb_[7]);
    // FFFFFFFFh plays from the current position (MMC §10.8.8).
    if (lba == 0xFFFFFFFF) lba = play_cur_lba_;
    if (len == 0) { command_complete(); return; }  // not an error, MMC §10.8.8
    const Track *start = track_at(lba);
    if (!start || !start->is_audio) {
        set_check(kKeyIllegalRequest, kAscIllegalModeForTrack, 0x00);
        return;
    }
    const uint32_t end = lba + len;
    if (!audio_range_stays_in_type(lba, end, /*is_audio=*/true)) {
        set_check(kKeyIllegalRequest, kAscEndOfUserArea, 0x00);
        return;
    }
    start_audio_playback(lba, end);
}

void AtapiCdrom::cmd_play_audio_msf() {
    // MMC §10.8.9. FFh FFh FFh means the current head position.
    auto msf_to_lba = [](const uint8_t *p) -> uint32_t {
        if (p[0] == 0xFF && p[1] == 0xFF && p[2] == 0xFF) return 0xFFFFFFFF;
        const uint32_t f = uint32_t(p[0]) * 60 * 75 + uint32_t(p[1]) * 75 + uint32_t(p[2]);
        return f >= 150 ? f - 150 : 0;  // undo the Red Book 2-second pregap offset
    };
    uint32_t start = msf_to_lba(&cdb_[3]);
    uint32_t end = msf_to_lba(&cdb_[6]);
    if (start == 0xFFFFFFFF) start = play_cur_lba_;
    if (end == 0xFFFFFFFF) end = play_cur_lba_;
    if (start == end) { command_complete(); return; }  // not an error, MMC §10.8.9
    if (start > end) {
        set_check(kKeyIllegalRequest, kAscInvalidFieldInCdb, 0x00);
        return;
    }
    const Track *t = track_at(start);
    if (!t || !t->is_audio) {
        set_check(kKeyIllegalRequest, kAscIllegalModeForTrack, 0x00);
        return;
    }
    if (!audio_range_stays_in_type(start, end, /*is_audio=*/true)) {
        set_check(kKeyIllegalRequest, kAscEndOfUserArea, 0x00);
        return;
    }
    start_audio_playback(start, end);
}

// True if every track overlapping [lba, end) matches want_audio.
bool AtapiCdrom::audio_range_stays_in_type(uint32_t lba, uint32_t end, bool want_audio) const {
    if (end > disc_end_lba()) return false;
    for (const auto &t : tracks_) {
        const uint32_t t_end = t.start_lba + t.length_lba;
        if (lba < t_end && end > t.start_lba && t.is_audio != want_audio) return false;
    }
    return true;
}

void AtapiCdrom::cmd_pause_resume() {
    // MMC §10.8.7: pausing a paused play or resuming a running one is not an error.
    const bool resume = (cdb_[8] & 0x01) != 0;
    if (!playing_audio_) {
        set_check(kKeyAbortedCommand, kAscPlayOperationAborted, 0x00);
        return;
    }
    if (resume) {
        audio_paused_ = false;
        audio_status_ = 0x11;  // playing
    } else {
        audio_paused_ = true;
        audio_status_ = 0x12;  // paused
    }
}

void AtapiCdrom::cmd_stop_play_scan() {
    stop_audio_playback(0x15);  // MMC Table 116: no current audio status
}

void AtapiCdrom::cmd_read_subchannel() {
    // MMC §10.8.18. SubQ=0 returns just the header.
    const bool msf = (cdb_[1] & 0x02) != 0;
    const bool subq = (cdb_[2] & 0x40) != 0;
    const uint8_t format = cdb_[3];
    const uint16_t alloc = be16(&cdb_[7]);
    if (!subq) {
        uint8_t s[4] = {};
        s[1] = audio_status_;
        respond(s, sizeof(s), alloc);
        return;
    }
    auto put_addr = [&](uint8_t *p, uint32_t lba) {
        if (!msf) { put_be32(p, lba); return; }
        const uint32_t f = lba + 150;  // Red Book 2-second pregap offset
        p[0] = 0;
        p[1] = uint8_t(f / (75 * 60));
        p[2] = uint8_t((f / 75) % 60);
        p[3] = uint8_t(f % 75);
    };
    if (format == 0x01) {
        // Current Position Data (Table 115), the only format tracked.
        uint8_t s[16] = {};
        s[1] = audio_status_;
        put_be16(s + 2, 12);  // sub-channel data length, header excluded
        s[4] = 0x01;          // sub-channel data format code
        const Track *t = track_at(play_cur_lba_);
        const int track_no = t ? t->number : (tracks_.empty() ? 1 : tracks_.back().number);
        s[5] = uint8_t(0x10 | (t && !t->is_audio ? 0x04 : 0x00));  // ADR=1, CONTROL per track type
        s[6] = uint8_t(track_no);
        s[7] = 1;  // index: sub-indices within a track aren't modeled
        put_addr(s + 8, play_cur_lba_);
        put_addr(s + 12, t ? (play_cur_lba_ - t->start_lba) : play_cur_lba_);
        respond(s, sizeof(s), alloc);
    } else if (format == 0x02 || format == 0x03) {
        // MCN / ISRC (Tables 119/121) not modeled, so MCVal/TCVal stay clear.
        uint8_t s[24] = {};
        s[1] = audio_status_;
        put_be16(s + 2, 20);
        s[4] = format;
        respond(s, sizeof(s), alloc);
    } else {
        set_check(kKeyIllegalRequest, kAscInvalidFieldInCdb, 0x00);
    }
}

// --- CD-DA playback ---
// Independent of the packet state machine, paced by cycle credit against 44.1kHz.

void AtapiCdrom::start_audio_playback(uint32_t start_lba, uint32_t end_lba) {
    playing_audio_ = true;
    audio_paused_ = false;
    audio_status_ = 0x11;  // playing
    play_cur_lba_ = start_lba;
    play_end_lba_ = end_lba;
    play_frame_in_lba_ = 0;
    audio_credit_ = 0.0;
    audio_prev_cycles_ = prev_cycles_;
}

void AtapiCdrom::stop_audio_playback(uint8_t status) {
    playing_audio_ = false;
    audio_paused_ = false;
    audio_status_ = status;
}

void AtapiCdrom::advance_audio(uint64_t cpu_cycles) {
    // The clock baseline advances while paused so resume gets no burst of samples.
    const uint64_t delta = cpu_cycles - audio_prev_cycles_;
    audio_prev_cycles_ = cpu_cycles;
    if (audio_paused_) return;
    audio_credit_ += double(delta);
    const double cycles_per_frame = cpu_hz_ / double(kAudioSampleRateHz);
    while (audio_credit_ >= cycles_per_frame) {
        if (play_cur_lba_ >= play_end_lba_) { stop_audio_playback(0x13); return; }  // completed
        const Track *t = track_at(play_cur_lba_);
        if (!t || !t->is_audio) { stop_audio_playback(0x14); return; }  // stopped due to error
        const uint64_t frame =
            t->pcm_base_frame + uint64_t(play_cur_lba_ - t->start_lba) * 588 + play_frame_in_lba_;
        const int16_t raw_l = audio_pcm_[frame * 2];
        const int16_t raw_r = audio_pcm_[frame * 2 + 1];
        audio_credit_ -= cycles_per_frame;
        // Back-dated like SoundBlaster's burst DMA so frames due in one call get spaced timestamps.
        if (audio_samples_.size() < kMaxAudioSamples) {
            audio_samples_.push_back(Sample{cpu_cycles - uint64_t(audio_credit_), raw_l, raw_r});
        }
        if (++play_frame_in_lba_ >= 588) {
            play_frame_in_lba_ = 0;
            ++play_cur_lba_;
        }
    }
}

std::vector<AtapiCdrom::Sample> AtapiCdrom::drain_samples() {
    std::vector<Sample> out;
    out.swap(audio_samples_);
    return out;
}

// Linear gain from the port volume byte (SFF-8020i Table 62), gated by the
// channel-selection nibble (Table 61).
float AtapiCdrom::audio_port_gain(int port, int channel) const {
    const AudioPort &p = audio_ports_[port];
    if (((p.channel_selection >> channel) & 0x01) == 0) return 0.0f;
    return float(p.volume) / 255.0f;
}

}  // namespace pc486
