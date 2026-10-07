// Address decode and jumper state, from pcb6502full.net's 74HC00 (U5) wiring.
// See CGOAC6502_REVIEW.md for the net-by-net evidence.
//
// Decode, as wired:
//   CS_EEP (ROM ~CS)  = NAND(A15,A15) = /A15         -> ROM   $8000-$FFFF
//   CS2               = NAND(A14, CS_EEP)            -> active $4000-$7FFF
//   VIA  ~CS2 && A13=1                                -> VIA   $6000-$7FFF
//   ACIA ~CS2 && A12=1                                -> ACIA  $5000-$5FFF
//   CS_RAM (as wired) = NAND(Phi2, CS_EEP)            -> RAM   all of A15=0
//
// Two deviations from the literal netlist, confirmed with the board's owner
// (CGOAC6502_REVIEW.md "RAM control-signal erratum", "VIA/ACIA overlap"):
//   1. RAM ~WE<-A14, ~OE<-R/W is treated as a schematic export error and
//      corrected to ~WE<-R/W. RAM is narrowed to $0000-$3FFF (bios.s banner:
//      "16K RAM") so it stays clear of the VIA/ACIA window.
//   2. VIA and ACIA selects both assert at $7000-$7FFF. Kept as a latent
//      bug the firmware never triggers; accesses hit both chips (see .cpp).
//
// J7 "Interrupts": bios.s (IRQ_HANDLER reads VIA T1CL, NMI_HANDLER reads
// ACIA_STATUS/DATA) shows VIAIRQ on CPU IRQ and ACIAIRQ on CPU NMI, the
// default here. With both jumpers removed neither line reaches the CPU.

#ifndef CG_OAC_6502_BUS_H
#define CG_OAC_6502_BUS_H

#include <cstdint>

#include "acia65c51.h"
#include "eeprom28c256.h"
#include "via65c22.h"

namespace bus {

struct JumperState {
    // J7: route each chip's IRQ to the CPU's IRQ or NMI pin, or neither
    enum class Route { None, Irq, Nmi };
    Route via_irq_route = Route::Irq;
    Route acia_irq_route = Route::Nmi;
    // J8: ACIA RTS to the RS232 CTS pin via the MAX232's spare channel
    bool rts_to_rs232_cts = false;
    // J5: PA1-4 boot-select jumpers, unread by the shipped ROM
    uint8_t boot_select = 0;
};

class Bus {
public:
    eeprom28c256::Eeprom rom;
    via65c22::Via via;
    acia65c51::Acia acia;
    JumperState jumpers;

    uint8_t ram[16384] = {};

    uint8_t read(uint16_t addr);
    void write(uint16_t addr, uint8_t v);

    // Set per access when the address is in the VIA/ACIA overlap ($7000-$7FFF)
    bool last_access_was_contended = false;

    // IRQ/NMI into the CPU after J7 routing
    bool irq_line() const;
    bool nmi_pending() const;
    void ack_nmi() { nmi_latched_ = false; }

    void tick(int cycles);
    void reset();

private:
    bool nmi_latched_ = false;
    bool via_irq_last_ = false, acia_irq_last_ = false;
};

} // namespace bus

#endif // CG_OAC_6502_BUS_H
