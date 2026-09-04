// Emscripten wrapper: binds the 8080 core + 88-2SIO board into one `Machine`
// object the browser can drive. Build with `make` in this directory (needs the
// emsdk toolchain on PATH).
//
// JS surface (all via embind):
//   const m = new Module.Machine();
//   m.reset();
//   m.loadBytes(uint8Array, 0x0000);   // drop a ROM / program into memory
//   m.sendByte(0x41);                   // terminal -> serial channel A
//   m.runCycles(33333);                 // advance ~1 frame at 2 MHz
//   const out = m.readOutput();         // Uint8Array the CPU transmitted
//   const s   = m.state();              // { a,b,c,...,pc,sp,halted,cycles }

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <array>
#include <cstdint>
#include <vector>

#include "../i8080.h"
#include "../serial2sio.h"
#include "../disk88.h"
#include "../disk_bootrom.h"
#include "../cassette.h"

using emscripten::val;

namespace {

// A tiny built-in program so the page does something before a ROM is loaded:
// reset ACIA channel A, configure 8N1, then echo every received byte.
const uint8_t kEchoRom[] = {
    0x3E, 0x03, 0xD3, 0x10,                    // MVI A,03 / OUT 10   master reset
    0x3E, 0x11, 0xD3, 0x10,                    // MVI A,11 / OUT 10   8N1, /16, IRQ off
    0xDB, 0x10, 0xE6, 0x01, 0xCA, 0x08, 0x00,  // wait RDRF
    0xDB, 0x11, 0x47,                          // IN 11 / MOV B,A
    0xDB, 0x10, 0xE6, 0x02, 0xCA, 0x12, 0x00,  // wait TDRE
    0x78, 0xD3, 0x11,                          // MOV A,B / OUT 11
    0xC3, 0x08, 0x00,                          // JMP loop
};

} // namespace

class Machine {
public:
    Machine() : cpu_(make_bus()) {
        load_default_rom();
        cpu_.reset();
        // The 88-2SIO's RX/TX-ready conditions jam RST 7 (vector 0x38) onto the
        // bus when interrupts are enabled -- the conventional Altair 2SIO
        // vector. Cpu::interrupt() is itself a no-op when disabled or mid-EI-
        // delay, so firing this on every status change needs no debouncing.
        // See ALTAIR_REVIEW.md §2.2/§2.3.
        sio_.on_irq = [this] { if (cpu_.interrupt(0xFF)) int_seen_ = true; };
    }

    // Wipe RAM and re-seed the built-in echo program.
    void reset() {
        mem_.fill(0);
        rom_lo_ = 0x10000; rom_hi_ = 0;
        load_default_rom();
        sio_.reset();
        disk_.reset();
        cassette_.reset();
        cpu_.reset();
    }

    // Clear RAM without seeding anything (call before loadBytes for a real ROM).
    void clearMemory() { mem_.fill(0); rom_lo_ = 0x10000; rom_hi_ = 0; }

    // Copy bytes from a JS Uint8Array (or array) into memory at `addr`. Bytes
    // that land above the current RAM ceiling become the read-only ROM window
    // (the 0xE000 BASIC image, a boot PROM) so a small-RAM machine can still
    // run them.
    void loadBytes(val bytes, int addr) {
        const unsigned len = bytes["length"].as<unsigned>();
        for (unsigned i = 0; i < len; ++i) {
            const unsigned a = static_cast<unsigned>(addr) + i;
            if (a >= mem_.size()) break;
            mem_[a] = bytes[i].as<uint8_t>();
        }
        const unsigned end = static_cast<unsigned>(addr) + (len ? len - 1 : 0);
        if (static_cast<unsigned>(addr) >= ram_top_ && len) {
            rom_lo_ = static_cast<unsigned>(addr);
            rom_hi_ = end < mem_.size() ? end : mem_.size() - 1;
        }
    }

    // Contiguous RAM from 0 (the Altair way). 4..64 KB.
    void setRam(int kb) {
        if (kb < 1) kb = 1;
        if (kb > 64) kb = 64;
        ram_top_ = static_cast<unsigned>(kb) * 1024;
    }
    int ramKb() const { return static_cast<int>(ram_top_ / 1024); }

    // The front panel's RESET/CLR paddle: a bus signal to the CPU and the
    // S-100 cards' electrical state, nothing more. The cassette deck isn't on
    // the bus -- a real Altair's RESET line doesn't reach a box plugged into
    // a completely separate cable, so it stays wherever it was and keeps
    // playing if PLAY is down (disk_.reset() deselecting the 88-DCDD *is*
    // correct: that card genuinely is on the bus). See ALTAIR_REVIEW.md §3.4.
    void reboot() {
        sio_.reset();
        disk_.reset();
        cpu_.reset();
    }

    // ---- 88-DCDD disk drives ------------------------------------------
    // Insert a diskette image (a flat 337,568-byte sector dump) into a drive.
    void mountDisk(int drive, val bytes) {
        std::vector<uint8_t> data =
            emscripten::convertJSArrayToNumberVector<uint8_t>(bytes);
        disk_.mount(drive, data.data(), data.size());
    }
    void unmountDisk(int drive)       { disk_.unmount(drive); }
    bool diskPresent(int drive) const { return disk_.mounted(drive); }
    bool diskDirty(int drive)   const { return disk_.dirty(drive); }
    void clearDiskDirty(int drive)    { disk_.clearDirty(drive); }

    // The current (possibly written-to) image, for "save disk to file".
    val diskImage(int drive) {
        const std::vector<uint8_t> &img = disk_.image(drive);
        val out = val::global("Uint8Array").new_(img.size());
        if (!img.empty())
            out.call<void>("set", val(emscripten::typed_memory_view(img.size(), img.data())));
        return out;
    }

    // Small snapshot for the drive-panel UI (polled roughly once per frame).
    val diskStatus() const {
        val o = val::object();
        o.set("selected",   disk_.selectedDrive());
        o.set("headLoaded", disk_.headLoaded());
        o.set("track0",     disk_.track(0));
        o.set("track1",     disk_.track(1));
        o.set("io",         static_cast<double>(disk_.ioTicks()));
        o.set("step",       static_cast<double>(disk_.stepTicks()));
        return o;
    }

    // Put the 88-DCDD bootstrap PROM at 0xFF00 (read-only) without touching RAM
    // or the CPU -- so EXAMINE 0FF00h / RUN on the panel finds real boot code,
    // exactly as it would on a machine with the DCDD controller fitted.
    void mapDiskBoot() {
        for (int i = 0; i < 256; ++i)
            mem_[altair::kDiskBootAddr + i] = altair::kDiskBootRom[i];
        if (rom_lo_ > altair::kDiskBootAddr) rom_lo_ = altair::kDiskBootAddr;
        if (rom_hi_ < altair::kDiskBootAddr + 255) rom_hi_ = altair::kDiskBootAddr + 255;
    }

    // Turnkey disk boot: drop the MITS 88-DCDD bootstrap PROM at 0xFF00 and
    // start there, exactly like flipping EXAMINE 0FF00h / RUN on the panel.
    void bootDisk() {
        mem_.fill(0);
        for (int i = 0; i < 256; ++i)
            mem_[altair::kDiskBootAddr + i] = altair::kDiskBootRom[i];
        rom_lo_ = altair::kDiskBootAddr;
        rom_hi_ = altair::kDiskBootAddr + 255;
        sio_.reset();
        cpu_.reset();
        cpu_.pc = altair::kDiskBootAddr;
    }

    // ---- 88-ACR cassette --------------------------------------------
    void mountTape(val bytes) {
        std::vector<uint8_t> d = emscripten::convertJSArrayToNumberVector<uint8_t>(bytes);
        cassette_.mount(d.data(), d.size());
    }
    void ejectTape()            { cassette_.eject(); }
    void rewindTape()           { cassette_.rewind(); }
    void setTapeMotor(bool on)     { cassette_.setMotor(on); }
    void setTapeRecordArm(bool on) { cassette_.setRecordArm(on); }
    void setTapeWind(int dir)      { cassette_.setWind(dir); }   // +1 FF, -1 REW, 0 release
    bool tapeLoaded()     const { return cassette_.loaded(); }
    bool tapeDirty()      const { return cassette_.dirty(); }
    void clearTapeDirty()       { cassette_.clearDirty(); }
    val tapeImage() {
        const std::vector<uint8_t> &d = cassette_.data();
        val out = val::global("Uint8Array").new_(d.size());
        if (!d.empty())
            out.call<void>("set", val(emscripten::typed_memory_view(d.size(), d.data())));
        return out;
    }
    val tapeStatus() const {
        val o = val::object();
        o.set("mode", static_cast<int>(cassette_.mode()));   // 0 idle, 1 play, 2 rec
        o.set("transport", cassette_.transport());           // 0 stop 1 play 2 rec 3 FF 4 REW
        o.set("pos",  static_cast<double>(cassette_.pos()));
        o.set("len",  static_cast<double>(cassette_.len()));
        o.set("cap",  static_cast<double>(cassette_.capacity()));
        o.set("io",   static_cast<double>(cassette_.ioTicks()));
        return o;
    }

    // Point the program counter at a program's entry (call after loadBytes +
    // reboot). CP/M .COM images start at 0x0100; most ROM images at 0x0000.
    void setPC(int addr) { cpu_.pc = static_cast<uint16_t>(addr & 0xFFFF); }

    // Front-panel sense switches, read via IN 0FFh. Altair BASIC checks these
    // at cold start to pick the console device; 0 selects the 2SIO.
    void setSenseSwitches(int v) { sense_ = static_cast<uint8_t>(v & 0xFF); }

    // Terminal -> serial channel A receive FIFO.
    void sendByte(int byte) { sio_.host_send(static_cast<uint8_t>(byte & 0xFF)); }

    // Everything the CPU has transmitted on channel A since the last call,
    // as a freshly allocated Uint8Array.
    val readOutput() {
        std::vector<uint8_t> bytes = sio_.host_drain();
        val out = val::global("Uint8Array").new_(bytes.size());
        if (!bytes.empty()) {
            out.call<void>(
                "set",
                val(emscripten::typed_memory_view(bytes.size(), bytes.data())));
        }
        return out;
    }

    // Run instructions until at least `cycles` T-states have elapsed.
    void runCycles(int cycles) {
        int64_t remaining = cycles;
        while (remaining > 0) remaining -= cpu_.step();
        disk_.tick(cpu_.cycles);   // advance the disk's rotational credit (§3.2d)
    }

    // Advance the cassette transport for one rendered frame. Call every frame
    // regardless of whether the CPU is running -- the deck is a separate box
    // with its own motor, not on the S-100 bus, so PLAY/FF/REW keep the reels
    // turning even with the front panel on STOP (or powered off). While the
    // CPU runs, the transport is paced by its own cycle clock (so 25x/50x
    // still track CPU-cycle time exactly, turbo included); while it isn't,
    // `idle_cycles_` advances the same clock by real elapsed time instead, at
    // the machine's native 2 MHz, so the two paces splice together with no
    // jump when RUN resumes. See ALTAIR_REVIEW.md §3.4a.
    void tickCassette(double dtMs, bool running) {
        if (!running) idle_cycles_ += static_cast<uint64_t>(dtMs * 2000.0);
        cassette_.tick(cpu_.cycles + idle_cycles_);
    }

    // Cassette transfer rate: 30 = the real 300 baud, 150 = 5x, 0 = unlimited.
    void setTapeSpeed(int bytesPerSec) { cassette_.setSpeed(bytesPerSec); }

    // Disk rotation rate: 193 = "Realistic" (~166 ms/rev over 32 sectors), 0 = unlimited.
    void setDiskSpeed(int sectorsPerSec) { disk_.setSpeed(sectorsPerSec); }

    // Execute exactly one instruction (front-panel SINGLE STEP).
    int stepOne() { return cpu_.step(); }

    // Front-panel EXAMINE / DEPOSIT peek and poke.
    int  readByte(int addr)          { return mem_[addr & 0xFFFF]; }
    void writeByte(int addr, int v)  { mem_[addr & 0xFFFF] = static_cast<uint8_t>(v & 0xFF); }

    bool   halted()      const { return cpu_.halted; }
    int    lastAddr()    const { return last_addr_; }   // last address on the bus

    // Per-address-bit touch counts since the last call (resets on read). The
    // real address lamps are incandescent bulbs that integrate brightness over
    // every bus cycle in a frame: a bit driven on nearly every cycle (Kill the
    // Bit's `D`, via four LDAX D per loop) glows visibly brighter than one a
    // slow counter only sweeps through once (`H`). An OR of "did this bit ever
    // go high" can't reproduce that -- see ALTAIR_REVIEW.md §3.6b.
    val busActivityCounts() {
        val out = val::global("Uint16Array").new_(addr_hits_.size());
        out.call<void>("set",
            val(emscripten::typed_memory_view(addr_hits_.size(), addr_hits_.data())));
        addr_hits_.fill(0);
        return out;
    }
    double cycleCount()   const { return static_cast<double>(cpu_.cycles); }
    unsigned rxPending()  const { return static_cast<unsigned>(sio_.rx_pending()); }
    unsigned txPending()  const { return static_cast<unsigned>(sio_.tx_pending()); }

    val state() {
        val o = val::object();
        o.set("a", cpu_.a);   o.set("b", cpu_.b);   o.set("c", cpu_.c);
        o.set("d", cpu_.d);   o.set("e", cpu_.e);
        o.set("h", cpu_.h);   o.set("l", cpu_.l);
        o.set("pc", cpu_.pc); o.set("sp", cpu_.sp);
        o.set("flags", cpu_.f);
        o.set("halted", cpu_.halted);
        o.set("intEnabled", cpu_.int_enabled);
        o.set("cycles", static_cast<double>(cpu_.cycles));
        // front-panel status lamps derived from the last bus access (§3.6a
        // above); "wo" is already unwrapped to the real active-low sense --
        // true means the WO line reads asserted (i.e. not a write). We don't
        // model HALT's own repeated internal fetch-discard cycles, so a
        // halted CPU reports no bus activity rather than leaving whatever
        // access preceded HALT stuck "on" forever.
        o.set("memr", !cpu_.halted && last_bus_op_ == BusOp::kMemRead);
        o.set("wo",   cpu_.halted || last_bus_op_ != BusOp::kMemWrite);
        o.set("inp",  !cpu_.halted && last_bus_op_ == BusOp::kIoIn);
        o.set("out",  !cpu_.halted && last_bus_op_ == BusOp::kIoOut);
        o.set("intAck", int_seen_);
        int_seen_ = false;   // pulse-stretched to "since last read", like busActivityCounts()
        return o;
    }

private:
    i8080::Bus make_bus() {
        i8080::Bus bus;
        bus.read  = [this](uint16_t a) -> uint8_t {
            touch(a);
            last_bus_op_ = BusOp::kMemRead;
            // the ROM window shadows RAM underneath it (real hardware: the PROM
            // decoder wins the bus regardless of how much RAM is installed) --
            // check it first so a 64 KB build doesn't let ram_top_ swallow it
            if (a >= rom_lo_ && a <= rom_hi_) return mem_[a];
            if (a < ram_top_)                 return mem_[a];
            return 0xFF;                            // unpopulated: floating high
        };
        bus.write = [this](uint16_t a, uint8_t v) {
            touch(a);
            last_bus_op_ = BusOp::kMemWrite;
            if (a >= rom_lo_ && a <= rom_hi_) return;   // ROM: writes don't stick
            if (a < ram_top_) mem_[a] = v;
        };
        bus.in    = [this](uint8_t port) -> uint8_t {
            last_bus_op_ = BusOp::kIoIn;
            if (sio_.owns(port))      return sio_.in(port);
            if (disk_.owns(port))     return disk_.in(port);
            if (cassette_.owns(port)) return cassette_.in(port);
            if (port == 0xFF)         return sense_;   // Altair front-panel sense switches
            return 0xFF;                               // unmapped port: floating bus
        };
        bus.out   = [this](uint8_t port, uint8_t v) {
            last_bus_op_ = BusOp::kIoOut;
            if (sio_.owns(port))      { sio_.out(port, v);      return; }
            if (disk_.owns(port))     { disk_.out(port, v);     return; }
            if (cassette_.owns(port)) { cassette_.out(port, v); return; }
        };
        return bus;
    }

    void load_default_rom() {
        for (unsigned i = 0; i < sizeof(kEchoRom); ++i) mem_[i] = kEchoRom[i];
    }

    // Declaration order matters: mem_ and sio_ are built before cpu_, whose
    // constructor calls make_bus() and captures them.
    std::array<uint8_t, 0x10000> mem_{};
    unsigned                      ram_top_ = 0x10000;   // contiguous RAM from 0
    unsigned                      rom_lo_  = 0x10000;    // read-only window above RAM
    unsigned                      rom_hi_  = 0;
    uint16_t                      last_addr_ = 0;
    std::array<uint16_t, 16>      addr_hits_{};   // per-bit touch counts since busActivityCounts()
    uint64_t                      idle_cycles_ = 0;   // see tickCassette()

    // MEMR/WO/INP/OUT for the front panel: which kind of bus access happened
    // most recently, held until the next one (the same "level held between
    // polls" trick busActivityCounts() uses for the address lamps). Real
    // hardware asserts these only for the few T-states of the matching
    // machine cycle; a once-a-frame poll can only ever see the last one.
    // M1 (opcode fetch, vs. a plain read for an operand byte) and STACK need
    // the CPU core itself to say what an access is *for*, not just which Bus
    // callback fired -- deliberately not modelled. HLDA/PROT are already
    // correct as permanently off: there's no DMA-capable peripheral to ever
    // assert HOLD, and the front panel's PROTECT paddle is a documented
    // no-op (see panel.spec.ts). See ALTAIR_REVIEW.md §3.6a.
    enum class BusOp { kNone, kMemRead, kMemWrite, kIoIn, kIoOut };
    BusOp last_bus_op_ = BusOp::kNone;
    bool  int_seen_    = false;   // an interrupt was accepted since the last state() read

    void touch(uint16_t a) {
        last_addr_ = a;
        for (int b = 0; b < 16; ++b)
            if ((a >> b) & 1) addr_hits_[b] += (addr_hits_[b] < 0xFFFF);
    }
    altair::Serial2SIO            sio_{0x10};
    altair::Disk88               disk_;
    altair::CassetteACR          cassette_;
    uint8_t                       sense_ = 0x00;  // 0 => console on the 2SIO
    i8080::Cpu                    cpu_;
};

EMSCRIPTEN_BINDINGS(retro8080) {
    emscripten::class_<Machine>("Machine")
        .constructor<>()
        .function("reset",       &Machine::reset)
        .function("reboot",      &Machine::reboot)
        .function("bootDisk",    &Machine::bootDisk)
        .function("mapDiskBoot", &Machine::mapDiskBoot)
        .function("mountDisk",   &Machine::mountDisk)
        .function("unmountDisk", &Machine::unmountDisk)
        .function("diskPresent", &Machine::diskPresent)
        .function("diskDirty",   &Machine::diskDirty)
        .function("clearDiskDirty", &Machine::clearDiskDirty)
        .function("diskImage",   &Machine::diskImage)
        .function("diskStatus",  &Machine::diskStatus)
        .function("setRam",      &Machine::setRam)
        .function("ramKb",       &Machine::ramKb)
        .function("mountTape",   &Machine::mountTape)
        .function("ejectTape",   &Machine::ejectTape)
        .function("rewindTape",  &Machine::rewindTape)
        .function("setTapeMotor", &Machine::setTapeMotor)
        .function("setTapeRecordArm", &Machine::setTapeRecordArm)
        .function("setTapeWind", &Machine::setTapeWind)
        .function("tapeLoaded",  &Machine::tapeLoaded)
        .function("tapeDirty",   &Machine::tapeDirty)
        .function("clearTapeDirty", &Machine::clearTapeDirty)
        .function("tapeImage",   &Machine::tapeImage)
        .function("tapeStatus",  &Machine::tapeStatus)
        .function("setTapeSpeed", &Machine::setTapeSpeed)
        .function("setDiskSpeed", &Machine::setDiskSpeed)
        .function("clearMemory", &Machine::clearMemory)
        .function("loadBytes",   &Machine::loadBytes)
        .function("setPC",       &Machine::setPC)
        .function("setSenseSwitches", &Machine::setSenseSwitches)
        .function("sendByte",    &Machine::sendByte)
        .function("readOutput",  &Machine::readOutput)
        .function("runCycles",   &Machine::runCycles)
        .function("tickCassette", &Machine::tickCassette)
        .function("stepOne",     &Machine::stepOne)
        .function("readByte",    &Machine::readByte)
        .function("writeByte",   &Machine::writeByte)
        .function("halted",      &Machine::halted)
        .function("lastAddr",    &Machine::lastAddr)
        .function("busActivityCounts", &Machine::busActivityCounts)
        .function("cycleCount",  &Machine::cycleCount)
        .function("rxPending",   &Machine::rxPending)
        .function("txPending",   &Machine::txPending)
        .function("state",       &Machine::state);
}
