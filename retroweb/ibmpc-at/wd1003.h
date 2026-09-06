// WD1003-WA2-compatible MFM hard disk controller.
//
// Ports 0x1F0-0x1F7 (command block) + 0x3F6 (control block; NOT 0x3F7 --
// real AT wiring deliberately doesn't put the HDD's device-address
// register there, since the floppy controller already owns 0x3F7), IRQ14 --
// the WD1003 is the direct ancestor of IDE/ATA, and by the time BIOS-level
// detection conventions were standardized, "WD1003-compatible" came to
// mean exactly this register layout. Drive C: ships pre-loaded with the
// system's OS, per CLAUDE.md's "Adding a new machine" rule -- there is no
// swap UI for a fixed disk, matching a real AT.
//
// Scope/simplifications (see IBM_PCAT_REVIEW.md):
//  - This machine's actual firmware substitute (BIOS-bochs-legacy)
//    detects and drives the disk as a genuine ATA/IDE device -- writing a
//    0x55/0xAA scratch pattern to Sector Count/Number and reading it back,
//    a soft reset via the Device Control register's SRST bit, checking the
//    post-reset signature (Cylinder Low/High + status), then issuing
//    IDENTIFY DEVICE (0xEC) and reading geometry directly out of its
//    response -- not the classic CMOS fixed-disk-parameter-table path. A
//    genuine 1984-vintage WD1003 predates IDENTIFY DEVICE (an ATA
//    addition from 1988+); implementing it here is the same kind of
//    labelled compatibility concession as cpu80286.h's 0x66 support --
//    not real WD1003-WA2 behavior, needed because the firmware substitute
//    assumes it. The same goes for 28-bit LBA addressing (an ATA-2/1996+
//    convention, selected via bit 6 of the Drive/Head register): this
//    BIOS's own disk-I/O routine converts every CHS request to LBA in
//    software and always issues ATA commands that way, so
//    offset_for_current_registers() below implements it.
//    Machine::configure_factory_cmos() *also* seeds the
//    classic CMOS "Type 47 user-definable" geometry bytes this same BIOS
//    separately reads into a legacy EBDA parameter table -- both paths
//    describe the identical ST-4038 geometry, so nothing is inconsistent.
//  - A READ SECTORS command is paced to the real MFM transfer rate
//    (~625,000 bytes/sec, a representative ST-506/412-interface figure)
//    via a single accumulated byte-credit target, then executed as one
//    bulk memory<->image copy -- the same "respect the real total transfer
//    TIME, not the byte-by-byte hardware handshake" tradeoff disk88.h and
//    fdc765.h already make, for the same reason (see fdc765.h's header).
//    WRITE SECTORS deliberately does NOT pace this way: it commits
//    synchronously the instant the CPU supplies the last byte. This is a
//    real ATA asymmetry (a drive can report a write done as soon as the
//    data lands in its buffer, but can't return read data before actually
//    fetching it off the platter first) that happens to be load-bearing
//    here -- this machine's real firmware substitute's write-completion
//    poll (rombios.c's ata_cmd_data_io()) never waits at all before
//    checking status, unlike its read-side poll. See pio_write_byte()'s
//    comment and IBM_PCAT_REVIEW.md.
//  - Multi-sector PIO transfers raise IRQ14 once, at the end of the whole
//    request, rather than once per sector the way real hardware's PIO
//    handshake does -- acceptable because DOS-era boot code overwhelmingly
//    reads one sector at a time anyway (a boot sector is exactly one).
#ifndef IBMPCAT_WD1003_H
#define IBMPCAT_WD1003_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ibmpcat {

class Wd1003 {
public:
    struct Drive {
        std::vector<uint8_t> image;
        bool present = false;
        bool dirty = false;
        int cylinders = 0, heads = 0, sectors_per_track = 0;

        static constexpr int kBytesPerSector = 512;
        long offset_for(int cyl, int head, int sector) const {
            return ((long(cyl) * heads + head) * sectors_per_track + (sector - 1)) * kBytesPerSector;
        }
        long capacity_sectors() const { return long(cylinders) * heads * sectors_per_track; }
    };
    Drive drives[2];  // 0 = C: (this system's only populated drive), 1 = unused (D:, always absent)

    Wd1003() { reset(); }

    void reset();

    bool owns(uint16_t port) const;
    uint8_t in(uint16_t port);
    void out(uint16_t port, uint8_t v);
    // The data register (0x1F0) additionally needs a genuine atomic
    // 16-bit path -- see cpu80286.h's Bus::in16/out16 comment.
    uint16_t data_in16();
    void data_out16(uint16_t v);

    void tick(uint64_t cpu_cycles);

    bool irq_pending() const { return irq_pending_; }

    // Host/front-end side: pre-load the drive at construction/setup time --
    // matching a real fixed disk that ships already formatted from the
    // factory, not a swappable-media device.
    void mount(int drive, const uint8_t *data, std::size_t len);

    // Host/front-end convenience: true while an actual command is being
    // serviced (BSY asserted) or a READ/WRITE SECTORS transfer is paced
    // and in flight -- what a real activity LED wired to the controller's
    // BSY/DRQ lines would light for.
    bool busy() const { return (status_ & ST_BSY) != 0 || xfer_active_; }

private:
    enum Status : uint8_t {
        ST_ERR = 0x01, ST_DRQ = 0x08, ST_DSC = 0x10, ST_DF = 0x20, ST_DRDY = 0x40, ST_BSY = 0x80,
    };
    enum class PioMode { kNone, kReadDrain, kWriteFill };

    uint8_t error_ = 0;
    uint16_t sector_count_ = 1;   // 0 conventionally means 256 in real ATA
    uint8_t sector_number_ = 1;
    uint8_t cyl_low_ = 0, cyl_high_ = 0;
    uint8_t drive_head_ = 0xA0;   // bits5,7 always 1 on real hardware
    uint8_t status_ = ST_DRDY | ST_DSC;
    // Real single-device-cable hardware fact, distinct from selected_drive_
    // itself: Cylinder Low/High and Status are driven by the one real
    // device (Sector Count/Number are too -- see out()'s comment -- but
    // those never need to look different per drive), EXCEPT for a brief
    // window right after a soft reset performed while the absent slave was
    // selected, where nothing drives them and they float to the bus's
    // pulled-up 0xFF. That's specifically the signal the real BIOS's
    // ata_detect() reads (Cylinder Low, then Cylinder High) to conclude
    // "no second drive" -- this counts exactly those two reads down from 2
    // to 0, after which Cylinder Low/High/Status genuinely reflect the
    // real device again regardless of what's selected (there's nothing
    // else to distinguish "device 0 selected" from "device 1 selected" on
    // this cable once that one detection window has passed -- see the real
    // boot-sector-read bug this fixed in IBM_PCAT_REVIEW.md, where the
    // BIOS's own boot loader reads status well after this window with the
    // slave still nominally selected and needs the real master value).
    int floating_reads_left_ = 0;
    bool nien_ = false;           // Device Control bit1: interrupts disabled to the host
    bool srst_prev_ = false;
    int selected_drive_ = 0;
    bool irq_pending_ = false;

    std::vector<uint8_t> pio_buffer_;
    std::size_t pio_pos_ = 0;
    PioMode pio_mode_ = PioMode::kNone;

    // Pending READ/WRITE-SECTORS transfer, paced like fdc765's.
    bool xfer_active_ = false;
    bool xfer_is_write_ = false;
    long xfer_offset_ = 0;
    std::size_t xfer_len_ = 0;
    double xfer_credit_ = 0.0, xfer_target_ = 0.0;
    uint64_t prev_cycles_ = 0;

    static constexpr double kCpuHz = 8000000.0;
    static constexpr double kBytesPerSec = 625000.0;  // representative ST-506/412-interface MFM rate

    // True when the currently-selected drive (via drive_head_ bit4) has no
    // hardware there at all. This is a fixed fact of this specific
    // machine's wiring, not a function of whether media happens to be
    // mounted yet: drive 0 is a permanently-installed fixed disk (always
    // physically present, per this file's header comment -- there's no
    // swap UI because a real AT's fixed disk isn't removable), and drive 1
    // (the slave/D: position) is permanently unpopulated. Deliberately NOT
    // keyed off Drive::present/mount(), which is purely about whether a
    // disk image happens to be loaded yet -- e.g. the existing register-
    // protocol tests exercise drive 0 without ever mounting an image, which
    // is realistic (the controller answers regardless of whether the
    // platters hold a valid filesystem).
    //
    // Gates the COMMAND REGISTER WRITE only (0x1F7, see out()) -- that's
    // the one register access that actually depends on device selection
    // on real ATA/IDE hardware, because it's what decides which device's
    // command-execution logic responds; a genuine single-device cable has
    // no separate Device 1 state machine to receive it, so a command
    // aimed at Device 1 (e.g. IDENTIFY DEVICE) simply never executes.
    // This is what fixes the real divide-by-zero bug this session's
    // install testing caught: the BIOS's ata_detect() scratch-register
    // test (write 0x55/0xAA, read it back) needs the IDENTIFY it issues
    // to a "second drive" to silently do nothing so it correctly
    // concludes "no second drive" -- otherwise it would go on to read
    // back a phantom device's all-zero IDENTIFY geometry.
    //
    // Every OTHER task-file register write (0x1F0-0x1F6, plus the always-
    // unconditional 0x3F6) is NOT gated by this, and neither is any read
    // (see in()) -- both are real ATA hardware facts, not simplifications:
    // Sector Count/Number/Cylinder Low/High/Features are simple latches on
    // a shared parallel bus that whichever device is physically present
    // absorbs regardless of the DEV bit (there's no per-device routing for
    // them at all), and a lone Device 0 with no Device 1 wired must keep
    // answering register reads regardless of the DEV bit too, since
    // there's nothing else on the bus for it to float to. Getting either
    // of these backwards was a real, and quite subtle, bug: this BIOS's
    // ata_detect() legally leaves the slave selected after probing it
    // last during POST, and its own boot-sector-read code programs the
    // CHS/sector-count registers *before* reselecting the master --
    // gating those writes on the then-still-selected (absent) slave
    // silently discarded the real request parameters, corrupting the
    // very first boot-sector read into a bogus, out-of-range one. See
    // IBM_PCAT_REVIEW.md.
    bool selected_drive_absent() const { return (selected_drive_ & 1) != 0; }

    // Byte offset into the selected drive's image implied by the current
    // Sector Number/Cylinder Low/Cylinder High/Drive-Head registers --
    // handles both classic CHS and 28-bit LBA addressing. See the .cpp
    // definition for why both are genuinely needed.
    long offset_for_current_registers() const;

    void run_command(uint8_t cmd);
    void do_identify();
    void begin_read();
    void begin_write();
    void finish_read_or_write();  // called once the paced transfer completes
    uint8_t pio_read_byte();
    void pio_write_byte(uint8_t v);
};

}  // namespace ibmpcat

#endif  // IBMPCAT_WD1003_H
