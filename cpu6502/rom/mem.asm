; OS variable memory addresses (zero page memory = faster)
CURRENT_COMMAND  = $00      ; Default is command 0
LOCAL_MEM_BYTE   = $00 + 1  ; Mem to use for local storage
PROMPT_CHAR_CNT  = $00 + 2  ; max command prompt length is 256
PROGRAM_PRT      = $00 + 3  ; 2 Bytes 

; 0100 - 01FF Reserved for Stack

PROMPT_START       = $0200  ; Command prompt, 256 bytes
PROGRAM_START_ADDR = $0300  ; Program start should be from here to the end of RAM (minus Wozmon buffer)
; 15,415 bytes of program RAM

; Wozmon Input buffer
; Must be at the end of RAM with appropriate buffer size
WOZMON_BUFFER    = $3F37      ; 200 bytes should be enough?? (3FFF - C8)
