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

// ATAPI/SCSI multi-byte CDB fields are big-endian (SPC §3.4.2) -- the
// opposite of every x86-side structure a DOS driver hands the device, which
// is why a real driver byte-swaps on its way into the CDB.
uint16_t be16(const uint8_t *p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }
uint32_t be32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void put_be16(uint8_t *p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v & 0xFF); }
void put_be32(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

// Space-padded, NOT NUL-terminated -- SCSI INQUIRY / ATA IDENTIFY string
// fields are both fixed-width blank-filled, and a driver that prints them
// raw shows the padding.
void put_padded(uint8_t *dst, std::size_t width, const char *s) {
    std::size_t len = std::strlen(s);
    for (std::size_t i = 0; i < width; ++i) dst[i] = uint8_t(i < len ? s[i] : ' ');
}

}  // namespace

void AtapiCdrom::reset() {
    assert_signature();
    // DRDY deliberately NOT set: ATA/ATAPI-4 §9.1's signature table gives a
    // packet device Status = 00h after reset. See the header.
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
    // Mode page 0Eh (SFF-8020i Table 60): ports 0/1 wired straight through
    // (channel 0 -> port 0, channel 1 -> port 1) at full volume by default
    // -- every real drive's factory wiring, so audio plays with no MODE
    // SELECT at all. Ports 2/3 are optional and default muted.
    audio_ports_[0] = AudioPort{0x1, 0xFF};
    audio_ports_[1] = AudioPort{0x2, 0xFF};
    audio_ports_[2] = AudioPort{0x0, 0x00};
    audio_ports_[3] = AudioPort{0x0, 0x00};
    // A reset raises a unit-attention condition (SPC §5.6): the first
    // command after it gets CHECK CONDITION / 06h / 29h 00h. Real drives do
    // this, and a real driver's REQUEST SENSE clears it -- which is exactly
    // why every DOS CD-ROM driver issues TEST UNIT READY twice at startup.
    set_unit_attention(kAscResetOccurred, 0x00);
    // image_/media_present_ survive: a reset does not eject the disc.
}

void AtapiCdrom::assert_signature() {
    // ATA/ATAPI-4 §9.1 "Signature and persistence": a device implementing
    // the PACKET command set reports Sector Count = 01h, Sector Number =
    // 01h, Cylinder Low = 14h, Cylinder High = EBh. An ATA disk reports
    // 01h/01h/00h/00h instead, and that difference is the entire basis of
    // device-type detection on an otherwise identical register block.
    int_reason_ = 0x01;
    sector_number_ = 0x01;
    byte_count_ = 0xEB14;
}

void AtapiCdrom::mount(const uint8_t *data, std::size_t len) {
    // Whole blocks only -- a real drive addresses 2048-byte sectors and
    // cannot read a partial one.
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
    // Lay out each track's bytes from the one BIN file. A single-FILE CUE
    // sheet's tracks are contiguous in the BIN by construction (that's the
    // whole point of one FILE line), so this walks a running byte cursor
    // rather than computing offsets from start_lba -- the two disagree
    // whenever a data and an audio track differ in bytes/sector. MODE1:
    // cooked 2048-byte user data into image_, concatenated in LBA order so
    // cmd_read10()'s existing flat addressing keeps working unchanged;
    // AUDIO: raw 2352-byte frames into audio_pcm_ as interleaved 16-bit
    // stereo samples, 588 frames/LBA.
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
    // The host/front-end path always opens the tray, even when a driver has
    // asked for it to be locked (PREVENT ALLOW MEDIUM REMOVAL) -- that's the
    // emergency-eject equivalent a real user has. The CDB path below honors
    // the lock, as a real drive does.
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
// "MM:SS:FF" -> absolute frame count (75 frames/sec, Red Book). Returns
// false on anything that doesn't parse, so a malformed CUE sheet is
// rejected rather than silently misread.
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

// CUE sheet subset: FILE (ignored -- the one BIN is the caller's own
// bin_data) / TRACK <n> <MODE1/2048|AUDIO> / INDEX <n> <MM:SS:FF>. Only
// INDEX 01 (the track's actual start, as opposed to INDEX 00's pre-gap) is
// used, matching real single-BIN game rips. Populates tracks_ with each
// track's number/type/whole-disc-LBA span; mount_cue() lays out the bytes.
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
        // FILE and anything else (REM, CATALOG, PREGAP...): not needed for
        // the single-BIN case this drive models, so ignored rather than
        // rejected -- a real-world CUE sheet carries plenty of both.
    }
    if (entries.empty()) return false;
    tracks_.clear();
    tracks_.reserve(entries.size());
    // Every track but the last has its length from the next INDEX 01's LBA
    // (sector-size-agnostic, since LBA is a time-based address). The last
    // track's length has to come from the BIN's remaining byte count
    // instead -- there's no next INDEX to subtract -- so this tracks a
    // running byte total in each track's OWN sector size as it goes,
    // rather than dividing the WHOLE bin_len by the last track's sector
    // size (which silently miscounts whenever an earlier track used a
    // different sector size, e.g. a MODE1/2048 track ahead of an AUDIO one).
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
        // "Device 0 responding for Device 1" -- ATA/ATAPI-6 (T13/1410D
        // revision 3a) Table 18. A PACKET device answers a read of Sector
        // Count, LBA Low/Mid/High, the Device register, Status or Alternate
        // Status with 00h while the absent device 1 is selected; only the
        // Error register still reports device 0's own contents. A read of
        // the Data register is an ignored bus cycle, which the host sees as
        // the bus's idle 0xFF. See selected_device_absent() for why this
        // differs from wd1003's model and what real driver depends on it.
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
    // Every register write lands in device 0's register regardless of the
    // DEV bit -- including the command register, which the device then
    // declines to act on: ATA/ATAPI-6 Table 18's command-register row reads
    // "Place new data into the Command register of Device 0. Do not respond
    // unless the command is EXECUTE DEVICE DIAGNOSTICS." That one exception
    // is real and deliberate: it is how a host collects a diagnostic result
    // covering both device positions from a channel where only one device
    // exists.
    if (port == 0x177 && selected_device_absent() && v != 0x90) return;
    switch (port) {
        case 0x170: write_data_byte(v); break;
        case 0x171: features_ = v; break;
        // Offsets 2-5 are shared latches (see the file header): the host
        // writes Sector Count / Sector Number / byte-count limit here, and
        // the device only overwrites them at a phase transition. The
        // legacy-BIOS 0x55/0xAA scratch probe depends on reading offsets
        // 2/3 back exactly as written.
        case 0x172: int_reason_ = v; break;
        case 0x173: sector_number_ = v; break;
        case 0x174: byte_count_ = uint16_t((byte_count_ & 0xFF00) | v); break;
        case 0x175: byte_count_ = uint16_t((byte_count_ & 0x00FF) | (uint16_t(v) << 8)); break;
        case 0x176: drive_head_ = v; selected_device_ = (v & 0x10) ? 1 : 0; break;
        case 0x177: run_command(v); break;
        case 0x376: {
            bool srst_now = (v & 0x04) != 0;
            if (srst_now && !srst_prev_) {
                // Soft reset, on the SRST 1->0 edge. The one real device on
                // this channel reasserts its own ATAPI signature whichever
                // device the DEV bit currently claims is selected: a write
                // to the Device Control register lands in device 0's
                // register and it "responds to the new values of the nIEN
                // and SRST bits" even while device 1 is selected
                // (ATA/ATAPI-6 Table 18). A probe that then reads the task
                // file with device 1 still selected gets 00h from the same
                // table's read rows, not this signature -- see in().
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
            // IDENTIFY DEVICE. ATA/ATAPI-4 §8.12.1: a device implementing
            // the PACKET command set shall abort this command and place the
            // ATAPI signature in the task file -- the second half of the
            // device-type detection mechanism, for a host that issues the
            // ATA identify before checking the reset signature.
            abort_command(/*with_signature=*/true);
            break;
        case 0x90:
            // EXECUTE DEVICE DIAGNOSTIC: pass, and reassert the signature
            // (ATA/ATAPI-4 §8.9 -- the diagnostic's completion is another
            // documented point at which the signature is valid).
            assert_signature();
            error_ = 0x01;
            status_ = ST_DRDY;
            irq_pending_ = !nien_;
            break;
        case 0xEF:
            // SET FEATURES: accepted. Drivers use it to select a PIO mode,
            // and there is nothing to change -- this device is PIO-only and
            // its timing is fixed.
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
    // ABRT in the Error register's low nibble, with the sense key in 7:4 --
    // ATA/ATAPI-4 §7.6.1's packet-device Error register layout.
    sense_key_ = kKeyIllegalRequest;
    asc_ = kAscInvalidOpcode;
    ascq_ = 0;
    check_pending_ = true;
    error_ = uint8_t((kKeyIllegalRequest << 4) | 0x04);
    status_ = ST_DRDY | ST_CHK;
    irq_pending_ = !nien_;
}

void AtapiCdrom::device_reset() {
    // ATA/ATAPI-4 §8.7: DEVICE RESET resets the packet device's protocol
    // state and reasserts the signature. It deliberately does NOT assert
    // INTRQ -- a driver polls BSY for its completion instead.
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
    // ATA string fields store each character pair byte-swapped within its
    // 16-bit word -- a real, documented convention (ATA/ATAPI-4 §8.12.8).
    auto put_string = [&](int word_start, int word_count, const char *s) {
        std::size_t len = std::strlen(s);
        for (int i = 0; i < word_count * 2; ++i) {
            char c = (std::size_t(i) < len) ? s[i] : ' ';
            std::size_t byte_idx = std::size_t(word_start) * 2 + std::size_t(i ^ 1);
            if (byte_idx < data_.size()) data_[byte_idx] = uint8_t(c);
        }
    };

    // Word 0, the general configuration word, is where this response
    // genuinely differs from an ATA disk's IDENTIFY DEVICE (wd1003's
    // do_identify() puts 0x0040 "fixed device" here). ATA/ATAPI-4 §8.13.8
    // Table 12:
    //   bits 15:14 = 10b  -> ATAPI device
    //   bits 12:8  = 00101b (05h) -> CD-ROM command set (SCSI-3 MMC)
    //   bit 7      = 1    -> removable media
    //   bits 6:5   = 10b  -> "accelerated" DRQ: DRQ comes up within 50us of
    //                        the PACKET command, and no interrupt is asserted
    //                        to request the command packet (see begin_packet())
    //   bits 1:0   = 00b  -> 12-byte command packet
    put16(0, 0x85C0);
    put_string(10, 10, "RW-CD-0001");         // serial number, words 10-19
    put_string(23, 4, "1.0");                 // firmware revision, words 23-26
    put_string(27, 20, "RETROWEB CD-ROM 2X"); // model number, words 27-46
    // Word 49 capabilities (ATA/ATAPI-4 §8.13.8): bit 9 = LBA supported,
    // bit 11 = IORDY supported. Bit 8 (DMA) is deliberately 0 -- this device
    // is PIO-only, and a driver that saw DMA advertised would go program a
    // bus-master controller that does not exist here.
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

    // IDENTIFY PACKET DEVICE uses the plain PIO data-in protocol, not the
    // packet protocol, so the byte-count limit does not apply: the whole
    // 512-byte block comes across in one DRQ burst.
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
        // DMA requested. Not supported (see the header); ATA/ATAPI-4 §9.6
        // has the device abort rather than silently fall back to PIO, so a
        // driver's negotiation actually notices.
        abort_command(/*with_signature=*/false);
        return;
    }
    // Byte count limit, from what the host left in offsets 4/5. ATA/ATAPI-4
    // §7.3.2: an odd limit is not usable, so the device uses the next lower
    // even value -- the classic case being a host that writes 0xFFFF and
    // gets 0xFFFE. A limit of zero is invalid; treat it as "no limit" rather
    // than deadlocking a driver that never set it.
    limit_ = byte_count_ & 0xFFFE;
    if (limit_ == 0) limit_ = 0xFFFE;

    cdb_pos_ = 0;
    std::memset(cdb_, 0, sizeof(cdb_));
    data_.clear();
    data_pos_ = block_end_ = 0;
    check_pending_ = false;
    error_ = 0;
    // C/D = 1, I/O = 0: "send me the command packet". DRQ up, BSY down, and
    // deliberately NO interrupt -- word 0's bits 6:5 declare this an
    // accelerated-DRQ device, and ATA/ATAPI-4 §9.6.1 has such a device not
    // assert INTRQ for the packet-request phase. A driver that waited for an
    // interrupt here instead of polling DRQ would hang, which is why the
    // DRQ-timing field in the identify data has to agree with this.
    int_reason_ = IR_CD;
    status_ = ST_DRDY | ST_DRQ;
    phase_ = Phase::kCommandPacket;
}

void AtapiCdrom::write_data_byte(uint8_t v) {
    if (phase_ == Phase::kDataOut) {
        // The one data-out CDB this drive accepts: MODE SELECT(10)'s
        // parameter list, set up by cmd_mode_select10(). Everything else
        // stays genuinely data-out-incapable -- see the file header.
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
    // The byte count register reports this block's size so the host knows
    // how many words to pull (ATA/ATAPI-4 §9.6.2) -- the direct analog of
    // what wd1003's IDENTIFY word 5 does for the disk's PIO loop.
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
    // ATA/ATAPI-4 §9.6: at command completion the device sets both I/O and
    // C/D and clears DRQ, so the Interrupt Reason register reads 03h --
    // "command complete", which is how a driver distinguishes the final
    // interrupt from a between-blocks one.
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
    // ATAPI transfers are counted in 16-bit words, so an odd length is
    // rounded up with a pad byte rather than leaving the host to read half a
    // word (ATA/ATAPI-4 §9.6.2).
    if (data_.size() & 1) data_.push_back(0);
}

void AtapiCdrom::execute_packet() {
    const uint8_t op = cdb_[0];
    double seconds = kCommandSec;
    check_pending_ = false;
    error_ = 0;
    data_.clear();
    data_pos_ = block_end_ = 0;

    // SPC §5.6: a pending unit-attention condition is reported as CHECK
    // CONDITION on the next command, and INQUIRY and REQUEST SENSE are
    // specifically exempt (a host has to be able to identify the drive and
    // read the sense data without the report getting in the way). This is
    // the path a DOS CD-ROM driver's media-change poll actually rides on.
    if (ua_pending_ && op != 0x12 && op != 0x03) {
        ua_pending_ = false;
        set_check(kKeyUnitAttention, ua_asc_, ua_ascq_);
        begin_execute(seconds);
        return;
    }

    // MMC Table 74/75: a PLAY AUDIO/SCAN operation in progress keeps
    // running across most commands (a real drive's audio output is its own
    // analog signal, independent of the ATA command stream), but a handful
    // of opcodes stop it first. This machine doesn't implement SCAN or
    // MECHANISM STATUS, so their rows don't apply; of the rest, everything
    // this drive implements other than the ones listed here belongs to
    // Table 75 ("will not stop a play or scan operation").
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
            // Everything else, including MODE SENSE(6)/MODE SELECT(6) -- the
            // 6-byte forms are genuinely absent from the ATAPI CD-ROM
            // command set (SFF-8020i defines only the 10-byte ones), so a
            // real drive answers them exactly this way and a driver probing
            // with them is supposed to fall back to the 10-byte form.
            set_check(kKeyIllegalRequest, kAscInvalidOpcode, 0x00, /*abrt=*/true);
            break;
    }
    begin_execute(seconds);
}

void AtapiCdrom::cmd_request_sense() {
    // SPC §7.20.2 fixed-format sense data, 18 bytes. REQUEST SENSE itself
    // never fails for "no media" -- it is the command a driver uses to find
    // out *why* something failed, so returning CHECK CONDITION here would
    // leave it with no way to learn anything.
    uint8_t s[18] = {};
    s[0] = 0x70;               // current error, information fields not valid
    s[2] = uint8_t(sense_key_ & 0x0F);
    s[7] = 10;                 // additional sense length -> 18 bytes total
    s[12] = asc_;
    s[13] = ascq_;
    respond(s, sizeof(s), cdb_[4]);
    // Reading the sense data clears it (SPC §7.20): the condition has been
    // reported, so the drive reverts to NO SENSE.
    sense_key_ = kKeyNoSense;
    asc_ = ascq_ = 0;
}

void AtapiCdrom::cmd_inquiry() {
    // SPC §7.5.2 standard INQUIRY data. Succeeds with no disc in the tray --
    // it describes the drive, not the medium, and a driver's detection pass
    // depends on that.
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
            // SPC §7.12: with medium removal prevented, an eject request is
            // refused rather than obeyed.
            set_check(kKeyIllegalRequest, kAscRemovalPrevented, 0x02);
            return;
        }
        if (media_present_) eject();
        return;
    }
    // LoEj + Start = close the tray. There is nothing to load if the host
    // has not mounted an image; the tray simply closes empty, and the next
    // media-requiring command reports NOT READY.
}

void AtapiCdrom::cmd_read_capacity() {
    // MMC READ CAPACITY returns the LAST valid LBA, not the block count --
    // an off-by-one a driver will faithfully propagate into its own volume
    // size if it is wrong here.
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
    // MMC SEEK(10): move the head to an LBA and return status only, no data.
    // Beyond the required command set, but FreeDOS's real ATAPICDD.SYS
    // issues it for its own DOS "seek" device command (Cmd_Seek), so
    // refusing it would fail a driver path that genuinely gets exercised.
    const uint32_t lba = be32(&cdb_[2]);
    if (lba >= capacity_blocks()) {
        set_check(kKeyIllegalRequest, kAscLbaOutOfRange, 0x00);
        return 0.0;
    }
    // A seek costs the real head-movement time for the distance actually
    // travelled, and leaves the head where a following READ(10) of that
    // same LBA can pick up without paying it again -- which is the entire
    // point of a driver issuing one.
    double seconds = access_seconds_for(lba);
    note_transfer(lba, 0);
    return seconds;
}

// Access time for a request starting at `lba`, given where the head is and
// what the read-ahead buffer already holds. See atapi_cdrom.h's
// kBufferSectors / kSeekMinSec for the model and its calibration.
double AtapiCdrom::access_seconds_for(uint32_t lba) const {
    // Cold drive (just reset, or media just changed): nothing is buffered and
    // the head is nowhere in particular, so charge the full published average.
    if (head_lba_ == kHeadUnknown) return kAccessSec;
    // Already in the read-ahead buffer: no head movement at all. This is the
    // case that dominates real DOS use of a CD -- a batch file, the utilities
    // it runs, and the ISO directory extents are all re-read constantly.
    const uint32_t start = head_lba_ > kBufferSectors ? head_lba_ - kBufferSectors : 0;
    if (lba >= start && lba <= head_lba_) return 0.0;
    const uint32_t cap = capacity_blocks();
    if (cap == 0) return kAccessSec;
    const uint32_t dist = lba > head_lba_ ? lba - head_lba_ : head_lba_ - lba;
    const double frac = double(dist) / double(cap);
    // Linear in the fraction of the disc crossed, from a short-seek floor up.
    // Calibrated so a one-third-stroke seek -- which is what a datasheet's
    // "average access time" figure actually means -- comes out at exactly
    // kAccessSec, the published 250 ms. Same "labelled model fitted to the
    // published endpoints" discipline PC486_REVIEW.md §4.6 uses for the CPU's
    // data-dependent instruction timings.
    return kSeekMinSec + (kAccessSec - kSeekMinSec) * 3.0 * frac;
}

// Records where the head ended up, which is also where the read-ahead buffer
// now ends.
void AtapiCdrom::note_transfer(uint32_t lba, uint32_t blocks) {
    head_lba_ = lba + blocks;
}

// End of the last track -- the lead-out address, and NOT the same thing as
// capacity_blocks() once a mixed-mode disc has audio tracks after the data
// track: capacity_blocks() stays the data track's own extent (what READ
// CAPACITY/READ(10)/SEEK(10) address), while the lead-out covers the whole
// disc.
uint32_t AtapiCdrom::disc_end_lba() const {
    return tracks_.empty() ? 0 : tracks_.back().start_lba + tracks_.back().length_lba;
}

void AtapiCdrom::cmd_read_toc() {
    // MMC READ TOC/PMA/ATIP, formats 0000b (the TOC) and 0001b (Session
    // Information). Beyond the required command set, but a real-mode CD-ROM
    // driver issues it during its own disc-present/disc-type check, so
    // leaving it out would leave that path untestable. Always exactly one
    // session; mount()'s plain-ISO path is the one-track-all-data case of
    // the same tracks_ list mount_cue() builds for a mixed-mode disc.
    const bool msf = (cdb_[1] & 0x02) != 0;
    const uint16_t alloc = be16(&cdb_[7]);
    const uint8_t format = uint8_t(cdb_[2] & 0x0F);
    if (format != 0x00 && format != 0x01) {
        set_check(kKeyIllegalRequest, kAscInvalidFieldInCdb, 0x00);
        return;
    }
    auto put_addr = [&](uint8_t *p, uint32_t lba) {
        if (!msf) { put_be32(p, lba); return; }
        // MSF form carries a 2-second (150 frame) pregap offset -- the
        // Red Book fact that LBA 0 is at 00:02:00, not 00:00:00.
        uint32_t f = lba + 150;
        p[0] = 0;
        p[1] = uint8_t(f / (75 * 60));
        p[2] = uint8_t((f / 75) % 60);
        p[3] = uint8_t(f % 75);
    };
    // ADR = 1 (position), CONTROL low bit clear for a data track (CDB_RBK
    // Q-subchannel control nibble, MMC Table 118): bit 2 of CONTROL is 1 for
    // data, 0 for two-channel audio -- so 0x14 (data) vs 0x10 (audio).
    auto control_for = [](bool is_audio) -> uint8_t { return is_audio ? 0x10 : 0x14; };
    if (format == 0x01) {
        // Session Information: a 4-byte header plus exactly one TOC track
        // descriptor, describing the first track of the last complete
        // session -- 12 bytes total, which is why a driver asking for this
        // format sets the allocation length to 0Ch. FreeDOS's UDVD2.SYS (the
        // driver CDROM.BAT actually prefers over ATAPICDD.SYS) issues
        // precisely `43 00 01 ... 00 0C` as its disc-present check, and
        // refusing it made the whole drive read as "not ready" to DOS. See
        // PC486_REVIEW.md §5.5.
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

// Appends page 0Eh, CD-ROM Audio Control Parameters (SFF-8020i Table 60):
// the live mirror of whatever cmd_mode_select10()/mode_select_apply() last
// set, which is what makes MODE SENSE/MODE SELECT round-trip for a driver
// that reads back what it wrote.
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
        // Page 2Ah, the CD Capabilities and Mechanical Status page (MMC /
        // SFF-8020i §10.8.6.4) -- the page a driver probes to learn the
        // drive's speed, loader type and whether it can eject. Answered
        // with no disc in the tray, since it describes the drive.
        std::size_t base = s.size();
        s.resize(base + 20, 0);
        uint8_t *p = s.data() + base;
        p[0] = 0x2A;  // page code, PS = 0
        // Page length 12h = 18 more bytes, for a 20-byte page total (SFF-8020i
        // Table 68: bytes 0-19). The previous value here, 14h, was simply
        // wrong -- it claimed 22 bytes while only 20 were ever written, which
        // this fix corrects alongside the CD-DA capability bits below.
        p[1] = 0x12;
        p[2] = 0x00;  // no CD-R/CD-RW/DVD read capability
        p[3] = 0x00;  // no write capability
        // Byte 4 bit 0 (Audio Play) is the one capability bit this drive
        // now genuinely has, via PLAY AUDIO(10)/MSF -- everything else
        // (Composite, Digital Port 1/2, Mode 2 Form 1/2, Multi Session)
        // stays clear: this drive implements READ(10) of MODE1 user data
        // and CD-DA playback, and nothing else.
        p[4] = 0x01;
        // Byte 5 bit 0 is a DIFFERENT capability -- "Red Book audio can be
        // read using the READ CD command" (MMC's own CD-DA-via-data-path),
        // which this drive does not implement (no READ CD/BCh) and so
        // stays honestly clear, same as the rest of this byte (no UPC,
        // ISRC, R-W subchannel, C2 pointers).
        p[5] = 0x00;
        // Loading mechanism type 001b (tray) in bits 7:5, Eject supported
        // (bit 3), Lock supported (bit 0).
        p[6] = 0x29;
        // Separate Volume (bit 0) and Separate Channel Mute (bit 1): real
        // now that mode page 0Eh's 4 independent output ports exist.
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
    // SPC MODE SELECT(10): Parameter List Length at cdb_[7..8]. This drive
    // accepts only page 0Eh's parameter list (16 bytes) in the one block a
    // real driver sends it in -- see mode_select_apply() for what happens
    // once the bytes arrive, and the file header for why every OTHER
    // data-out CDB stays genuinely unimplemented.
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
    // The mode parameter list is an 8-byte header (block descriptor length
    // always 0 on this drive, so nothing to skip beyond the header itself)
    // followed by the one mode page this drive recognizes for MODE SELECT.
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
    // "FFFF FFFFh shall implement audio play from the current location of
    // the optics" (MMC §10.8.8).
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
    // MMC §10.8.9. "FFh FFh FFh shall be taken from the Current Optical
    // Head location" (play_cur_lba_ serves as that, same as PLAY AUDIO(10)'s
    // FFFF FFFFh above).
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

// True if every track overlapping [lba, end) -- and the disc's own extent --
// agrees with `want_audio`. Used instead of scanning every individual LBA:
// tracks_ is a short list (a handful of entries at most), so this is the
// same check MMC's "information type changes within transfer length" rule
// describes, done per-track rather than per-block.
bool AtapiCdrom::audio_range_stays_in_type(uint32_t lba, uint32_t end, bool want_audio) const {
    if (end > disc_end_lba()) return false;
    for (const auto &t : tracks_) {
        const uint32_t t_end = t.start_lba + t.length_lba;
        if (lba < t_end && end > t.start_lba && t.is_audio != want_audio) return false;
    }
    return true;
}

void AtapiCdrom::cmd_pause_resume() {
    // MMC §10.8.7. Pausing an already-paused play, or resuming a play
    // that's already running, is explicitly "shall not be considered an
    // error" -- only pausing/resuming with no play operation active is.
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
    // MMC §10.8.18. The sub-Q bit (cdb_[2] bit 6) governs whether any
    // sub-channel data block follows the 4-byte header at all -- SubQ=0
    // returning just the header is explicitly "not an error".
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
        // CD-ROM Current Position Data (Table 115): the only format this
        // drive tracks live state for.
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
        // Media Catalogue Number / Track ISRC (Tables 119/121): neither is
        // modeled, so MCVal/TCVal stay clear -- an honest "not detected"
        // rather than fabricated data, same posture as MODE SENSE's
        // capabilities page.
        uint8_t s[24] = {};
        s[1] = audio_status_;
        put_be16(s + 2, 20);
        s[4] = format;
        respond(s, sizeof(s), alloc);
    } else {
        set_check(kKeyIllegalRequest, kAscInvalidFieldInCdb, 0x00);
    }
}

// --- CD-DA playback engine -------------------------------------------------
// Independent of the packet-protocol state machine above: a real drive's
// audio output is its own analog signal that plays in the background while
// the host is free to issue other commands (MMC Table 74/75; see
// execute_packet()'s stop-list). Paced the same way SoundBlaster::advance()
// paces digitized playback -- accumulated CPU-cycle credit against a fixed
// sample rate -- but one frame at a time rather than in bursts, since CD-DA
// has no DMA-driven byte-move step to batch.

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
    // The clock baseline always moves forward, whether or not audio is
    // paused: a pause has to mean "no credit accrues," not "time stops
    // being tracked," or a long pause would hand back a false burst of
    // elapsed cycles -- and therefore samples -- the instant playback
    // resumes.
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
        // Back-dated the same way SoundBlaster's burst DMA is: when several
        // frames become due in one call (tick() isn't guaranteed to run at
        // 44.1kHz granularity), each still gets its own, correctly spaced
        // timestamp rather than all sharing cpu_cycles -- audio_credit_ is
        // exactly how far "in the future" of this frame's real due time
        // cpu_cycles is, so subtracting it back-dates correctly.
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

// Linear gain from an output port's volume byte (SFF-8020i Table 62: FFh =
// 0 dB/unity, 00h = mute, and the 2 dB/6 dB-step attenuation table in
// between is exactly byte/255 on a linear amplitude scale -- e.g. 80h =
// 128/255 = 0.502 versus the table's published -6 dB = 10^(-6/20) = 0.501),
// gated by whether this port's channel-selection nibble (Table 61) routes
// the given Red Book channel (0 = left, 1 = right) to it at all.
float AtapiCdrom::audio_port_gain(int port, int channel) const {
    const AudioPort &p = audio_ports_[port];
    if (((p.channel_selection >> channel) & 0x01) == 0) return 0.0f;
    return float(p.volume) / 255.0f;
}

}  // namespace pc486
