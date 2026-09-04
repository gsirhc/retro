// MITS Altair 88-DCDD 8-inch floppy disk controller.
//
// The 88-DCDD board hangs up to 16 daisy-chained Pertec FD-400 drives off three
// I/O ports (octal 10/11/12 = 0x08/0x09/0x0A):
//
//   OUT 0x08  select + enable a drive (bit 7 = deselect)
//   IN  0x08  drive/controller status        (bits are ACTIVE LOW)
//   OUT 0x09  drive function: step, head load/unload, start write
//   IN  0x09  sector position register       (advances one sector per read)
//   OUT 0x0A  write data byte
//   IN  0x0A  read data byte
//
// A diskette is 77 tracks x 32 sectors x 137 bytes = 337,568 bytes, stored as a
// flat sector dump (the on-disk sector framing — sync byte, track/sector header,
// stop byte, checksum — is part of the image; the BIOS deals with it).
//
// The logic here is a faithful port of Charles E. Owen's altair_dsk.c from
// SIMH (permissively licensed): reading the sector-position port advances to
// the next sector, which is exactly what the real MITS boot PROM and CP/M
// BIOS are written against.
//
// Rotation IS timed, opt-in: setSpeed()/tick() gate that advance on a
// sectors-per-second credit, the same shape as CassetteACR's byte credit.
// The default -- setSpeed() never called, tick() never called -- is
// unlimited (every read instantly advances), which is what every caller that
// predates this got and still gets: the native CP/M diagnostic/boot harnesses
// and the GoogleTest suite never call tick(), so they're unaffected. Only the
// browser build opts in, via a LOAD SPEED selector matching the paper-tape
// and cassette pattern. See ALTAIR_REVIEW.md §3.2d.

#ifndef EMULATOR8080_DISK88_H
#define EMULATOR8080_DISK88_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace altair {

class Disk88 {
public:
    static constexpr int      kDrives     = 16;
    static constexpr int      kTracks     = 77;
    static constexpr int      kSectors    = 32;
    static constexpr int      kSectorLen  = 137;
    static constexpr std::size_t kImageSize = std::size_t(kTracks) * kSectors * kSectorLen;

    Disk88() { reset(); }

    // Ports 0x08..0x0A.
    bool owns(uint8_t port) const { return port >= 0x08 && port <= 0x0A; }
    uint8_t in(uint8_t port);
    void    out(uint8_t port, uint8_t value);

    void reset();

    // Rotation timing: sectors/sec credited toward the next IN 0x09 advance;
    // 0 = unlimited ("Max" / every caller before this existed). Real 88-DCDD:
    // ~166 ms/revolution over 32 sectors ~= 193/sec ("Realistic").
    void setSpeed(int sectorsPerSec) {
        cycles_per_sector_ = sectorsPerSec > 0 ? (2000000ull / static_cast<uint64_t>(sectorsPerSec)) : 0;
        credit_ = 0;
    }
    // Advance the rotational clock this is measured against. Call once per
    // frame from the host; harnesses that never call it keep the pre-timing,
    // always-instant behavior (credit_ defaults to "full").
    void tick(uint64_t cpuCycles);

    // ---- host / front-end side ----------------------------------------
    // Insert a diskette. `len` should be kImageSize; short images are padded,
    // long ones truncated. Returns false if the drive index is out of range.
    bool mount(int drive, const uint8_t *data, std::size_t len);
    void unmount(int drive);
    bool mounted(int drive) const {
        return drive >= 0 && drive < kDrives && !drives_[drive].image.empty();
    }
    // The (possibly modified) image, for persisting writes back to the picker.
    const std::vector<uint8_t> &image(int drive) const { return drives_[drive].image; }

    // For the drive-panel UI.
    int      selectedDrive() const { return selected_; }
    bool     headLoaded()    const { return selected_ >= 0 && (flags_ & 0x04); }
    int      track(int drive) const { return (drive >= 0 && drive < kDrives) ? drives_[drive].track : 0; }
    uint64_t ioTicks()   const { return io_ticks_; }    // bumps on every data byte
    uint64_t stepTicks() const { return step_ticks_; }  // bumps on every head step
    bool     dirty(int drive) const {
        return drive >= 0 && drive < kDrives && drives_[drive].dirty_since_mount;
    }
    void clearDirty(int drive) {
        if (drive >= 0 && drive < kDrives) drives_[drive].dirty_since_mount = false;
    }

private:
    struct Drive {
        std::vector<uint8_t> image;             // empty => no diskette
        int  track = 0;
        bool dirty_since_mount = false;
    };

    void flushWrite();                          // commit the sector buffer to the image

    Drive   drives_[kDrives];
    int     selected_ = -1;                     // -1 => controller disabled
    uint8_t flags_    = 0;                      // status, stored 1=true (returned inverted)
    int     sector_   = -1;                     // current sector, -1 => "not yet indexed"
    int     bufpos_   = 255;                    // read/write pointer inside sector_buf_
    bool    write_dirty_ = false;               // sector_buf_ holds bytes not yet flushed
    uint8_t sector_buf_[kSectorLen + 1] = {0};

    uint64_t io_ticks_   = 0;
    uint64_t step_ticks_ = 0;

    double   credit_            = 1e9;   // sector-advance credit; huge => unlimited by default
    uint64_t prev_tick_cy_      = 0;
    uint64_t cycles_per_sector_ = 0;      // 0 = unlimited
};

} // namespace altair

#endif // EMULATOR8080_DISK88_H
