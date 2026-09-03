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

    void reboot() {
        sio_.reset();
        disk_.reset();
        cassette_.reset();
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
        cassette_.tick(cpu_.cycles);   // advance the cassette-transport throttle
    }

    // Cassette transfer rate: 30 = the real 300 baud, 150 = 5x, 0 = unlimited.
    void setTapeSpeed(int bytesPerSec) { cassette_.setSpeed(bytesPerSec); }

    // Execute exactly one instruction (front-panel SINGLE STEP).
    int stepOne() { return cpu_.step(); }

    // Front-panel EXAMINE / DEPOSIT peek and poke.
    int  readByte(int addr)          { return mem_[addr & 0xFFFF]; }
    void writeByte(int addr, int v)  { mem_[addr & 0xFFFF] = static_cast<uint8_t>(v & 0xFF); }

    bool   halted()      const { return cpu_.halted; }
    int    lastAddr()    const { return last_addr_; }   // last address on the bus
    // OR of every address the bus touched since the last call -- the "blur" the
    // real address lamps show while the CPU runs (a tight loop lights the
    // addresses it hits; Kill the Bit's moving bit rides A8..A15). Resets on read.
    int    busActivity()       { int v = addr_or_; addr_or_ = 0; return v; }
    double cycleCount()   const { return static_cast<double>(cpu_.cycles); }
    unsigned rxPending()  const { return static_cast<unsigned>(sio_.rx_pending()); }
    unsigned txPending()  const { return static_cast<unsigned>(sio_.tx_pending()); }

    val state() const {
        val o = val::object();
        o.set("a", cpu_.a);   o.set("b", cpu_.b);   o.set("c", cpu_.c);
        o.set("d", cpu_.d);   o.set("e", cpu_.e);
        o.set("h", cpu_.h);   o.set("l", cpu_.l);
        o.set("pc", cpu_.pc); o.set("sp", cpu_.sp);
        o.set("flags", cpu_.f);
        o.set("halted", cpu_.halted);
        o.set("intEnabled", cpu_.int_enabled);
        o.set("cycles", static_cast<double>(cpu_.cycles));
        return o;
    }

private:
    i8080::Bus make_bus() {
        i8080::Bus bus;
        bus.read  = [this](uint16_t a) -> uint8_t {
            last_addr_ = a;
            addr_or_ |= a;
            if (a < ram_top_)                return mem_[a];
            if (a >= rom_lo_ && a <= rom_hi_) return mem_[a];
            return 0xFF;                            // unpopulated: floating high
        };
        bus.write = [this](uint16_t a, uint8_t v) { last_addr_ = a; addr_or_ |= a; if (a < ram_top_) mem_[a] = v; };
        bus.in    = [this](uint8_t port) -> uint8_t {
            if (sio_.owns(port))      return sio_.in(port);
            if (disk_.owns(port))     return disk_.in(port);
            if (cassette_.owns(port)) return cassette_.in(port);
            if (port == 0xFF)         return sense_;   // Altair front-panel sense switches
            return 0xFF;                               // unmapped port: floating bus
        };
        bus.out   = [this](uint8_t port, uint8_t v) {
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
    uint16_t                      addr_or_ = 0;   // address-bus blur since busActivity()
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
        .function("clearMemory", &Machine::clearMemory)
        .function("loadBytes",   &Machine::loadBytes)
        .function("setPC",       &Machine::setPC)
        .function("setSenseSwitches", &Machine::setSenseSwitches)
        .function("sendByte",    &Machine::sendByte)
        .function("readOutput",  &Machine::readOutput)
        .function("runCycles",   &Machine::runCycles)
        .function("stepOne",     &Machine::stepOne)
        .function("readByte",    &Machine::readByte)
        .function("writeByte",   &Machine::writeByte)
        .function("halted",      &Machine::halted)
        .function("lastAddr",    &Machine::lastAddr)
        .function("busActivity", &Machine::busActivity)
        .function("cycleCount",  &Machine::cycleCount)
        .function("rxPending",   &Machine::rxPending)
        .function("txPending",   &Machine::txPending)
        .function("state",       &Machine::state);
}
