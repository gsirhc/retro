; OS control memory addresses
DEBUG_BREAK      = $0000
DEBUG_A_VAL      = $0000 + 1
DEBUG_X_VAL      = $0000 + 2
DEBUG_Y_VAL      = $0000 + 3
DEBUG_C_VAL      = $0000 + 4
DEBUG_PROG       = $0000 + 5
; space for more vars, though it doesn't really matter
COMMAND          = $0000 + 6
PROGRAM_COUNTER  = $0000 + 7

; Always last as the user's program memory is stored from here to the end of RAM
START_MEM        = $0000 + 8

; Wozmon Input buffer
; Must be at the end of RAM with appropriate buffer size
WOZMON_BUFFER    = $3F37 ; 200 bytes should be enough?? (3FFF - C8)

; COMMAND values
LIST_CMD  = %00000001
RUN_CMD   = %00000010
NO_CMD    = %11111111

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
