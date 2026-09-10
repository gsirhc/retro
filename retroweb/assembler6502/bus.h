// Address decode, reset supervisor, and jumper-header state for the board
// -- derived directly from pcb6502full.net's 74HC00 (U5) gate wiring, not
// guessed. See CGOAC6502_REVIEW.md for the full net-by-net evidence trail.
//
// Decode, as wired:
//   CS_EEP (ROM ~CS)  = NAND(A15,A15) = /A15         -> ROM   $8000-$FFFF
//   CS2               = NAND(A14, CS_EEP)            -> active $4000-$7FFF
//   VIA  ~CS2 && A13=1                                -> VIA   $6000-$7FFF
//   ACIA ~CS2 && A12=1                                -> ACIA  $5000-$5FFF
//   CS_RAM (as wired) = NAND(Phi2, CS_EEP)            -> RAM   all of A15=0
//
// Two corrections vs. the literal netlist, both confirmed with the board's
// owner (see CGOAC6502_REVIEW.md "RAM control-signal erratum" and "VIA/ACIA
// overlap"):
//   1. RAM's real ~WE/~OE wiring (~WE<-A14, ~OE<-R/W) is treated as a
//      schematic export error and corrected here to the standard,
//      functional ~WE<-R/W -- and RAM's decoded range is narrowed to
//      $0000-$3FFF (matching what rom/bios.s's own boot banner claims:
//      "16K RAM") instead of the full A15=0 span the raw CS_RAM equation
//      implies, so it no longer contends with the VIA/ACIA window.
//   2. VIA and ACIA's selects still both assert at $7000-$7FFF (neither
//      excludes the other's qualifying bit) -- kept as a documented,
//      never-triggered-by-firmware latent bug: reads/writes in that range
//      hit both chips at once, modeled as real bus contention (see .cpp).
//
// J7 "Interrupts" jumper: confirmed from rom/bios.s (IRQ_HANDLER reads
// VIA's T1CL; NMI_HANDLER reads ACIA_STATUS/DATA) that the board, as used,
// has VIAIRQ wired to CPU IRQ and ACIAIRQ wired to CPU NMI -- that's this
// bus's default. Without R1/R2's pull-ups doing their job (both jumpers
// removed), neither line reaches the CPU at all; both are exposed as
// runtime-togglable, matching the real header.

#ifndef CG_OAC_6502_BUS_H
#define CG_OAC_6502_BUS_H

#include <cstdint>

#include "acia65c51.h"
#include "eeprom28c256.h"
#include "via65c22.h"

namespace bus {

struct JumperState {
    // J7: route each chip's IRQ output to the CPU's IRQ or NMI pin, or
    // leave it disconnected (R1/R2 pull the CPU pin high either way).
    enum class Route { None, Irq, Nmi };
    Route via_irq_route = Route::Irq;    // default: matches the shipped ROM's IRQ_HANDLER
    Route acia_irq_route = Route::Nmi;   // default: matches the shipped ROM's NMI_HANDLER
    // J8: ACIA's own RTS output routed out to the RS232 connector's CTS
    // pin via the MAX232's spare channel. Open (false) by default -- no
    // trace populates this, it's jumper wire only.
    bool rts_to_rs232_cts = false;
    // J5: PA1-4 boot-select jumpers. Wired up but unread by the shipped
    // ROM (see review doc) -- exposed for experimentation.
    uint8_t boot_select = 0;
};

class Bus {
public:
    eeprom28c256::Eeprom rom;
    via65c22::Via via;
    acia65c51::Acia acia;
    JumperState jumpers;

    uint8_t ram[16384] = {};   // $0000-$3FFF -- see file header, correction 1

    uint8_t read(uint16_t addr);
    void write(uint16_t addr, uint8_t v);

    // Recomputed each access: true if the address falls in the documented
    // VIA/ACIA overlap ($7000-$7FFF) -- surfaced for logging/UI, not just
    // internal bookkeeping.
    bool last_access_was_contended = false;

    // IRQ/NMI lines into the CPU, after J7 routing and R1/R2's pull-ups.
    bool irq_line() const;
    bool nmi_pending() const;   // edge-consumed by the caller each poll -- see .cpp
    void ack_nmi() { nmi_latched_ = false; }

    void tick(int cycles);   // advances VIA/ACIA timing
    void reset();

private:
    bool nmi_latched_ = false;
    bool via_irq_last_ = false, acia_irq_last_ = false;
};

} // namespace bus

#endif // CG_OAC_6502_BUS_H
