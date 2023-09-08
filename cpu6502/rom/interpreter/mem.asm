; OS control memory addresses
DEBUG_BREAK      = $0000
DEBUG_A_VAL      = $0000 + 1
DEBUG_X_VAL      = $0000 + 2
DEBUG_Y_VAL      = $0000 + 3
DEBUG_C_VAL      = $0000 + 4
CURRENT_COMMAND  = $0000 + 5
PROMPT_CNT       = $0000 + 6  ; max command prompt length is 256
PROGRAM_PRT      = $0000 + 7

PROMPT_PTR       = $0000 + 8  ; Command prompt, 256 bytes - variable mem
MAX_PROMPT       = $00FF      ; 1 less then program start

PROGRAM_PTR      = $0100      ; Program start, should be from here to the end of RAM (minus Wozmon)
MAX_PROGRAM      = $3F35      ; 1 less the Wozmon

; Wozmon Input buffer
; Must be at the end of RAM with appropriate buffer size
WOZMON_BUFFER    = $3F37      ; 200 bytes should be enough?? (3FFF - C8)

ACIA_DATA = $5000
ACIA_STATUS = $5001
ACIA_CMD = $5002
ACIA_CTRL = $5003

PORTB = $6000
PORTA = $6001
DDRB = $6002
DDRA = $6003
PCR = $600c     ; peripheral control reg
IFR = $600d     ; interrupt flag reg
IER = $600e     ; interrupt enable reg

E  = %10000000
RW = %01000000
RS = %00100000

WOZMON_PRG_START = $ff00

; COMMAND values
PROMPT_CMD   = %00000000
RESET_CMD    = %00000001
LOAD_CMD     = %00000010
RUN_CMD      = %00000011
WOZMON_CMD   = %00000100
