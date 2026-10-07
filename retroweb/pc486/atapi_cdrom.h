// ATAPI CD-ROM on the secondary IDE channel: 0x170-0x177, 0x376, IRQ15.
// Sole device (master, 2x, tray loader); device 1 is absent.
// ATA/ATAPI-4 (NCITS 317-1998), SFF-8020i / MMC, SPC for the shared commands.
//  - Post-reset signature: Cyl Low 0x14, Cyl High 0xEB, count/number 1,
//    Status 0x00 (ATA/ATAPI-4 §9.1). DRDY stays clear until a command completes.
//  - Status bits 4/5 are SERVICE/DMA-READY, not DSC (§7.15.6.3).
//  - Error register carries the sense key in bits 7:4 (§7.6.1).
//  - Offset 2 and offsets 4/5 are shared latches (Sector Count / Interrupt
//    Reason, byte-count limit / actual count). BIOS detection writes 0x55/0xAA
//    to offsets 2/3 and needs them to read back.
//  - IDENTIFY DEVICE aborts with the ATAPI signature (§8.12.1).
//  - Unit attention on reset (ASC 29h) and media change (ASC 28h), reported as
//    CHECK CONDITION on the next command except INQUIRY/REQUEST SENSE (SPC §5.6).
// PIO only. Data-in is paced to 2x timing (307,200 B/s, ~250 ms access) with
// one cycle-credit target. No WRITE(10); MODE SELECT(10) accepts page 0Eh only.
// CD-DA playback works on CUE+BIN audio tracks; READ CD is not implemented.
#ifndef PC486_ATAPI_CDROM_H
#define PC486_ATAPI_CDROM_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pc486 {

class AtapiCdrom {
public:
    // Mode 1 user data; READ CAPACITY reports 2048, not 512.
    static constexpr int kBytesPerSector = 2048;

    AtapiCdrom() { reset(); }

    void reset();

    bool owns(uint16_t port) const;
    uint8_t in(uint16_t port);
    void out(uint16_t port, uint8_t v);
    // Atomic 16-bit data register path, same as the primary channel's 0x1F0.
    uint16_t data_in16();
    void data_out16(uint16_t v);

    // Inline: called after every instruction.
    void tick(uint64_t cpu_cycles) {
        uint64_t delta = cpu_cycles - prev_cycles_;
        prev_cycles_ = cpu_cycles;
        if (exec_active_) {
            exec_credit_ += double(delta);
            if (exec_credit_ >= exec_target_) finish_execute();
        }
        // Audio plays independently of packet execution (MMC Table 74/75).
        if (playing_audio_) advance_audio(cpu_cycles);
    }

    bool irq_pending() const { return irq_pending_; }

    // `data` is a plain 2048-byte-sector ISO 9660 image, single data track.
    void mount(const uint8_t *data, std::size_t len);
    // `cue_text` is a CUE sheet (FILE/TRACK/INDEX only) naming the one BIN.
    // MODE1/2048 tracks are cooked 2048-byte sectors; AUDIO tracks are raw
    // 2352-byte frames of 16-bit/44.1kHz stereo PCM. False if it doesn't parse.
    bool mount_cue(const char *cue_text, const uint8_t *bin_data, std::size_t bin_len);
    void eject();
    bool media_present() const { return media_present_; }

    // CD audio reaches the sound card over the analog cable, not the ATA bus.
    // Same {cpu_cycle, left, right} log shape as SoundBlaster::Sample.
    struct Sample {
        uint64_t cpu_cycle;
        int16_t left, right;
    };
    std::vector<Sample> drain_samples();
    static constexpr uint32_t kAudioSampleRateHz = 44100;  // Red Book, fixed
    bool playing_audio() const { return playing_audio_ && !audio_paused_; }

    // True while a command is being serviced (activity LED).
    bool busy() const { return (status_ & ST_BSY) != 0 || exec_active_; }

    // Tracks Machine::cpu_hz() so command times stay correct under Turbo.
    void set_cpu_hz(double hz);

private:
    // Packet-device status (ATA/ATAPI-4 §7.15.6.3): bit 0 is CHK, bits 4/5 are
    // SERVICE/DMA-READY.
    enum Status : uint8_t {
        ST_CHK = 0x01, ST_DRQ = 0x08, ST_SERV = 0x10, ST_DMRD = 0x20,
        ST_DRDY = 0x40, ST_BSY = 0x80,
    };
    // Interrupt Reason, ATA/ATAPI-4 §7.12.
    enum IntReason : uint8_t {
        IR_CD = 0x01,   // 1 = a command packet is being transferred
        IR_IO = 0x02,   // 1 = transfer is to the host, 0 = to the device
        IR_REL = 0x04,  // bus release (overlap mode; never set here)
    };
    enum class Phase {
        kIdle,
        kCommandPacket,  // DRQ up, awaiting the 12-byte CDB via the data register
        kExecuting,      // BSY up, paced to real drive timing
        kDataIn,         // DRQ up, host draining one data block
        kDataOut,        // DRQ up, host sending one data block (MODE SELECT only)
    };

    // --- task file ------------------------------------------------------
    uint8_t error_ = 0x01;        // post-reset diagnostic code; sense key in 7:4 after a failure
    uint8_t features_ = 0;
    // Shared latches, see file header.
    uint8_t int_reason_ = 0x01;
    uint8_t sector_number_ = 0x01;
    uint16_t byte_count_ = 0xEB14;  // low = 0x14, high = 0xEB: the ATAPI signature
    uint8_t drive_head_ = 0xA0;     // bits 5,7 always 1, same legacy fact as wd1003's
    uint8_t status_ = 0x00;         // DRDY deliberately clear after reset -- see header

    bool nien_ = false;   // Device Control bit 1: interrupts masked to the host
    bool srst_prev_ = false;
    int selected_device_ = 0;
    bool irq_pending_ = false;

    // Device 0 responding for Device 1 (ATA/ATAPI-6 Table 18). For a PACKET
    // device, reads of count/LBA/Device and Status return 00h (unlike wd1003's
    // disk model), Error reads device 0's, and writes land in device 0.
    // ATAPICDD.SYS probes device 1 with 0x55/0xAA; echoing latches would
    // invent a phantom slave.
    bool selected_device_absent() const { return (selected_device_ & 1) != 0; }

    // --- packet / data state --------------------------------------------
    Phase phase_ = Phase::kIdle;
    uint8_t cdb_[12] = {};
    int cdb_pos_ = 0;
    uint16_t limit_ = 0;   // host's per-DRQ-block byte count limit, captured at PACKET

    std::vector<uint8_t> data_;
    std::size_t data_pos_ = 0;
    std::size_t block_end_ = 0;  // end of the DRQ block currently being drained

    // --- sense data (SPC fixed-format) ----------------------------------
    uint8_t sense_key_ = 0, asc_ = 0, ascq_ = 0;
    bool check_pending_ = false;
    bool ua_pending_ = false;
    uint8_t ua_asc_ = 0, ua_ascq_ = 0;
    bool locked_ = false;  // PREVENT ALLOW MEDIUM REMOVAL

    // --- media ----------------------------------------------------------
    std::vector<uint8_t> image_;
    bool media_present_ = false;

    // start_lba/length_lba are in the disc's 2352-byte frame LBA space. Audio
    // samples live in audio_pcm_ from pcm_base_frame (588 sample pairs per LBA).
    struct Track {
        int number = 1;
        bool is_audio = false;
        uint32_t start_lba = 0;
        uint32_t length_lba = 0;
        uint64_t pcm_base_frame = 0;
    };
    std::vector<Track> tracks_;
    std::vector<int16_t> audio_pcm_;  // interleaved L/R, all audio tracks concatenated
    const Track *track_at(uint32_t lba) const;
    const Track *track_number(int n) const;
    uint32_t disc_end_lba() const;
    bool parse_cue(const char *cue_text, std::size_t bin_len);

    // --- pacing ---------------------------------------------------------
    bool exec_active_ = false;
    double exec_credit_ = 0.0, exec_target_ = 0.0;
    uint64_t prev_cycles_ = 0;
    // kHeadUnknown: cold, nothing buffered.
    static constexpr uint32_t kHeadUnknown = 0xFFFFFFFF;
    uint32_t head_lba_ = kHeadUnknown;

    // Updated by set_cpu_hz().
    static constexpr double kCpuHz = 66000000.0;
    double cpu_hz_ = kCpuHz;
    // 1x is 75 sectors/s of 2048 bytes; 2x doubles it.
    static constexpr double kBytesPerSec = 307200.0;
    // Period 2x datasheets quote 250-350 ms, a one-third-stroke seek.
    static constexpr double kAccessSec = 0.25;
    // Short-seek floor; period drives quote 80-150 ms.
    static constexpr double kSeekMinSec = 0.08;
    // 32 sectors = 64KB, the low end of period 2x buffers (64-256KB).
    static constexpr uint32_t kBufferSectors = 32;
    // Decode/answer overhead even for TEST UNIT READY.
    static constexpr double kCommandSec = 0.001;

    // --- CD-DA audio playback --------------------------------------------
    // audio_status_ is MMC Table 116: 11h playing, 12h paused, 13h completed,
    // 14h error, 15h none (post-reset/STOP).
    bool playing_audio_ = false;
    bool audio_paused_ = false;
    uint8_t audio_status_ = 0x15;
    uint32_t play_cur_lba_ = 0;
    uint32_t play_end_lba_ = 0;  // exclusive
    uint32_t play_frame_in_lba_ = 0;  // stereo-sample-pair offset within play_cur_lba_'s 588
    double audio_credit_ = 0.0;
    uint64_t audio_prev_cycles_ = 0;
    std::vector<Sample> audio_samples_;
    // Bounds the log if the front end stops draining.
    static constexpr std::size_t kMaxAudioSamples = 1u << 16;

    // Audio Control Parameters (mode page 0Eh, SFF-8020i §10.8.6.1): four
    // output ports, each with a channel select (Table 61) and attenuation
    // (Table 62). Ports 0/1 default to full volume, 2/3 muted (Table 60).
    struct AudioPort {
        uint8_t channel_selection = 0;
        uint8_t volume = 0;
    };
    AudioPort audio_ports_[4];
    float audio_port_gain(int port, int channel) const;

    void advance_audio(uint64_t cpu_cycles);
    void start_audio_playback(uint32_t start_lba, uint32_t end_lba);
    void stop_audio_playback(uint8_t status);
    bool audio_range_stays_in_type(uint32_t lba, uint32_t end, bool want_audio) const;

    double access_seconds_for(uint32_t lba) const;
    void note_transfer(uint32_t lba, uint32_t blocks);

    void assert_signature();
    void run_command(uint8_t cmd);
    void identify_packet_device();
    void device_reset();
    void abort_command(bool with_signature);

    void begin_packet();
    void execute_packet();
    void begin_execute(double seconds);
    void finish_execute();
    void start_data_in_block();
    void command_complete();

    uint8_t read_data_byte();
    void write_data_byte(uint8_t v);
    void mode_select_apply();  // parses the data-out block write_data_byte() collected

    // set_check() also loads the Error register sense-key nibble.
    void set_check(uint8_t key, uint8_t asc, uint8_t ascq, bool abrt = false);
    void set_unit_attention(uint8_t asc, uint8_t ascq);
    bool require_media();
    void respond(const uint8_t *bytes, std::size_t len, std::size_t alloc);

    uint32_t capacity_blocks() const;

    // cmd_read10() returns extra transfer time on top of kCommandSec.
    void cmd_request_sense();
    void cmd_inquiry();
    void cmd_start_stop_unit();
    void cmd_read_capacity();
    double cmd_read10();
    double cmd_seek10();
    void cmd_read_toc();
    void cmd_mode_sense10();
    void append_audio_control_page(std::vector<uint8_t> *out) const;
    void cmd_mode_select10();
    void cmd_play_audio10();
    void cmd_play_audio_msf();
    void cmd_pause_resume();
    void cmd_stop_play_scan();
    void cmd_read_subchannel();
};

}  // namespace pc486

#endif  // PC486_ATAPI_CDROM_H
