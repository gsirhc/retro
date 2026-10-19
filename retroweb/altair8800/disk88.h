// MITS 88-DCDD floppy controller and Pertec FD-400 drives, ports 0x08..0x0A
// (MITS "Altair Floppy Disk Drive and Controller Hardware Documentation", 1977).

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
    static constexpr int      kRealSectorsPerSec = 192;   // 360 rpm x 32 hard sectors

    Disk88() { reset(); }

    bool owns(uint8_t port) const { return port >= 0x08 && port <= 0x0A; }
    uint8_t in(uint8_t port);
    void    out(uint8_t port, uint8_t value);

    void reset();

    // Disk rotation rate in sectors/sec: kRealSectorsPerSec is the real drive,
    // a multiple of it shortens rotational and seek waits only, 0 = unlimited.
    void setSpeed(int sectorsPerSec);
    // CPU cycle count at the next in()/out(); a timed drive needs it first.
    void clock(uint64_t cpuCycles) { pending_cy_ = cpuCycles; has_pending_ = true; }
    void tick(uint64_t cpuCycles);
    void setInte(bool on) { inte_ = on; }

    // `len` should be kImageSize; short images are padded, long ones truncated.
    // Returns false if the drive index is out of range.
    bool mount(int drive, const uint8_t* data, std::size_t len);
    void unmount(int drive);
    bool mounted(int drive) const {
        return drive >= 0 && drive < kDrives && !drives_[drive].image.empty();
    }
    const std::vector<uint8_t>& image(int drive) const { return drives_[drive].image; }

    int      selectedDrive() const { return selected_; }
    bool     headLoaded()    const { return selected_ >= 0 && head_loaded_; }
    int      track(int drive) const { return (drive >= 0 && drive < kDrives) ? drives_[drive].track : 0; }
    uint64_t ioTicks()   const { return io_ticks_; }
    uint64_t stepTicks() const { return step_ticks_; }
    bool     dirty(int drive) const {
        return drive >= 0 && drive < kDrives && drives_[drive].dirty_since_mount;
    }
    void clearDirty(int drive) {
        if (drive >= 0 && drive < kDrives) drives_[drive].dirty_since_mount = false;
    }

private:
    struct Drive {
        std::vector<uint8_t> image;
        int     track = 0;
        int64_t moved_at = -(int64_t(1) << 40);
        bool    dirty_since_mount = false;
    };

    bool timed() const { return rate_ > 0; }
    bool enabled() const { return selected_ >= 0; }
    void clear();
    void advance(bool scalable);
    void move_to(int64_t target);
    bool in_transfer() const;
    int64_t next_event() const;
    int  read_index() const;
    bool nrda() const;
    bool move_ok() const;
    bool head_ready() const;
    bool track0() const;
    void step(bool in);
    void start_write();
    void end_write(bool fill);
    void put(int drive, int track, int sector, int pos, uint8_t v);
    uint8_t get(int drive, int track, int sector, int pos) const;

    Drive   drives_[kDrives];
    int     selected_    = -1;
    bool    head_loaded_ = false;
    bool    inte_        = false;

    double   rate_        = 0;
    int64_t  now_         = 0;
    uint64_t last_cy_     = 0;
    bool     has_last_    = false;
    uint64_t pending_cy_  = 0;
    bool     has_pending_ = false;

    int64_t step_at_      = -(int64_t(1) << 40);
    int64_t head_mh_until_ = 0;
    int64_t settle_until_  = 0;
    int64_t write_end_at_  = -(int64_t(1) << 40);
    int64_t xfer_sector_   = -1;

    int64_t rd_sector_ = -1;
    int     rd_index_  = -1;
    uint8_t rd_latch_  = 0;

    bool    w_active_ = false;
    int     w_drive_  = 0;
    int64_t w_sector_ = 0;
    int64_t w_first_  = 0;
    int     w_slot_   = 0;
    uint8_t w_latch_  = 0;
    bool    w_fresh_  = false;
    bool    w_any_    = false;

    int     u_sector_ = kSectors - 1;
    bool    u_valid_  = false;
    int     u_pos_    = 0;

    uint64_t io_ticks_   = 0;
    uint64_t step_ticks_ = 0;
};

} // namespace altair

#endif // EMULATOR8080_DISK88_H
