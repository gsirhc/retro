// MITS Altair 88-DCDD 8-inch floppy controller, up to 16 Pertec FD-400 drives
// on ports 0x08..0x0A (octal 10/11/12). Status bits are active low.
// A diskette is 77 tracks x 32 sectors x 137 bytes, a flat dump that includes
// the on-disk sector framing. Logic ported from Charles E. Owen's altair_dsk.c
// (SIMH). Rotation timing is opt-in via setSpeed()/tick(); callers that never
// call tick() get instant sector advance.

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

    bool owns(uint8_t port) const { return port >= 0x08 && port <= 0x0A; }
    uint8_t in(uint8_t port);
    void    out(uint8_t port, uint8_t value);

    void reset();

    // Sectors/sec credited toward the next IN 0x09 advance; 0 = unlimited.
    // Real 88-DCDD: ~166 ms/rev over 32 sectors, ~193/sec.
    void setSpeed(int sectorsPerSec) {
        cycles_per_sector_ = sectorsPerSec > 0 ? (2000000ull / static_cast<uint64_t>(sectorsPerSec)) : 0;
        credit_ = 0;
    }
    void tick(uint64_t cpuCycles);

    // ---- host / front-end side ----------------------------------------
    // `len` should be kImageSize; short images are padded, long ones truncated.
    // Returns false if the drive index is out of range.
    bool mount(int drive, const uint8_t *data, std::size_t len);
    void unmount(int drive);
    bool mounted(int drive) const {
        return drive >= 0 && drive < kDrives && !drives_[drive].image.empty();
    }
    // The (possibly modified) image, for persisting writes back to the picker.
    const std::vector<uint8_t> &image(int drive) const { return drives_[drive].image; }

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
