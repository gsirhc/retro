// WD1003-WA2-compatible MFM hard disk controller. Ports 0x1F0-0x1F7 (command
// block) and 0x3F6 (control block), IRQ14. The floppy controller owns 0x3F7.
// Drive C: ships preloaded with the OS and has no swap UI, like a real AT.
//
// Simplifications (IBM_PCAT_REVIEW.md):
//  - BIOS-bochs-legacy drives the disk as ATA: 0x55/0xAA scratch test, SRST
//    soft reset, signature check, then IDENTIFY DEVICE (0xEC) for geometry.
//    IDENTIFY (ATA, 1988+) and 28-bit LBA (bit 6 of Drive/Head, ATA-2) postdate
//    the real WD1003. They are compatibility concessions for the firmware
//    substitute; see offset_for_current_registers(). Machine::configure_factory_cmos()
//    also seeds the Type 47 CMOS geometry that the same BIOS reads separately.
//  - READ SECTORS is paced to ~625,000 bytes/sec (a representative
//    ST-506/412 MFM rate), then done as one bulk copy.
//  - WRITE SECTORS commits synchronously when the last byte arrives. A drive
//    can report a write done once data is buffered, and the firmware's write
//    poll (rombios.c ata_cmd_data_io()) never waits. See pio_write_byte().
//  - Multi-sector PIO raises IRQ14 once at the end of the request, not per sector.
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
    Drive drives[2];  // 0 = C: (the only populated drive), 1 = D: (always absent)

    Wd1003() { reset(); }

    void reset();

    bool owns(uint16_t port) const;
    uint8_t in(uint16_t port);
    void out(uint16_t port, uint8_t v);
    // The data register (0x1F0) needs an atomic 16-bit path (cpu80286.h Bus::in16/out16).
    uint16_t data_in16();
    void data_out16(uint16_t v);

    void tick(uint64_t cpu_cycles);

    bool irq_pending() const { return irq_pending_; }

    // Preloads the drive at setup, like a factory-formatted fixed disk.
    void mount(int drive, const uint8_t *data, std::size_t len);

    // True while a command is serviced (BSY) or a paced transfer is in flight.
    bool busy() const { return (status_ & ST_BSY) != 0 || xfer_active_; }

    // C:'s current image and whether it changed since mount(), so the front end
    // can persist writes across a power cycle.
    const std::vector<uint8_t> &image(int drive) const { return drives[drive & 1].image; }
    bool dirty(int drive) const { return drives[drive & 1].dirty; }
    void clear_dirty(int drive) { drives[drive & 1].dirty = false; }

private:
    enum Status : uint8_t {
        ST_ERR = 0x01, ST_DRQ = 0x08, ST_DSC = 0x10, ST_DF = 0x20, ST_DRDY = 0x40, ST_BSY = 0x80,
    };
    enum class PioMode { kNone, kReadDrain, kWriteFill };

    uint8_t error_ = 0;
    uint16_t sector_count_ = 1;   // 0 means 256 in ATA
    uint8_t sector_number_ = 1;
    uint8_t cyl_low_ = 0, cyl_high_ = 0;
    uint8_t drive_head_ = 0xA0;   // bits 5 and 7 always read 1
    uint8_t status_ = ST_DRDY | ST_DSC;
    // Cylinder Low/High and Status come from the one real device, except for a
    // window after a soft reset with the absent slave selected, when they float
    // to 0xFF. ata_detect() reads Cylinder Low then High to conclude "no second
    // drive", so this counts those two reads down from 2. Afterwards the real
    // device answers whatever is selected (IBM_PCAT_REVIEW.md, boot-sector-read bug).
    int floating_reads_left_ = 0;
    bool nien_ = false;           // Device Control bit1: host interrupts disabled
    bool srst_prev_ = false;
    int selected_drive_ = 0;
    bool irq_pending_ = false;

    std::vector<uint8_t> pio_buffer_;
    std::size_t pio_pos_ = 0;
    PioMode pio_mode_ = PioMode::kNone;

    // Pending READ/WRITE SECTORS transfer, paced like fdc765's
    bool xfer_active_ = false;
    bool xfer_is_write_ = false;
    long xfer_offset_ = 0;
    std::size_t xfer_len_ = 0;
    double xfer_credit_ = 0.0, xfer_target_ = 0.0;
    uint64_t prev_cycles_ = 0;

    static constexpr double kCpuHz = 8000000.0;
    static constexpr double kBytesPerSec = 625000.0;  // representative ST-506/412 MFM rate

    // True when drive_head_ bit4 selects the unpopulated slave. Fixed by wiring,
    // not by Drive::present, since drive 0 answers even with no image mounted.
    //
    // Gates only command-register writes (0x1F7), as on a single-device ATA
    // cable: a command aimed at Device 1 never executes. ata_detect() relies on
    // this, since its IDENTIFY to the "second drive" must do nothing or the BIOS
    // reads a phantom all-zero geometry.
    //
    // Other task-file writes (0x1F0-0x1F6, 0x3F6) and all reads are ungated, as on
    // real ATA. ata_detect() leaves the slave selected and the boot-sector read
    // programs CHS/count before reselecting the master, so gating those writes
    // corrupted the first boot read (IBM_PCAT_REVIEW.md).
    bool selected_drive_absent() const { return (selected_drive_ & 1) != 0; }

    // Byte offset implied by Sector Number, Cylinder Low/High and Drive/Head,
    // for both CHS and 28-bit LBA.
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
