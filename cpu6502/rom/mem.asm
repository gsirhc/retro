; OS variable memory addresses (zero page memory = faster)
PROMPT_CHAR_CNT  = $00      ; max command prompt length is 256
PROGRAM_SIZE     = $00 + 1  ; 2 bytes
ENABLE_CRTL_C    = $00 + 3  ; 1 byte
TIME_JIFFIES     = $00 + 4  ; 1 byte
TIME_SECONDS     = $00 + 5  ; 1 byte
TIME_MINUTES     = $00 + 6  ; 1 byte
TIME_HOURS       = $00 + 7  ; 1 byte
JIFFY_TIMER      = $00 + 8  ; 1 byte

; 0000 - 004F Reserved OS Zero Page (minus fixed addresses above)
; 0050 - 00FF Reserved User Zero Page
; 0100 - 01FF Reserved for Stack

PROMPT_START       = $0200   ; Command prompt, 256 bytes
PROGRAM_START_ADDR = $0300   ; Program start should be from here to the end of RAM (minus Wozmon buffer)
; 15,415 bytes of program RAM


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;
;; OS Local Variables to help keep track of them for any routines that use multiple of the
;;  calls.  Otherwise it doesn't matter what a routine uses to store "local" variables or
;;  for input/output.
;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

; convert
DEC_VALUE        = $20                 ; 2 bytes
MOD10_VALUE      = $20 + 2             ; 2 bytes
DECIMAL_STR      = $20 + 4             ; 6 bytes (64K max + null termination)

; math
DIVIDE_VALUE     = $30                 ; 2 bytes
DIVISOR          = $30 + 2             ; 1-2 bytes (supports 8 bit and 16 bit division)
REMAINDER        = $30 + 4             ; 1-2 bytes (supports 8 bit and 16 bit division)

; printProg
PRT_PROGRAM_PRT  = $3E                  ; current pointer to read next byte, 2 bytes

; exec
ADDRESS          = $4D                 ; Address store, 2 bytes
TEMP_STOR        = $4F                 ; dont have 4 registers, so store in available zero page

; Wozmon Input buffer
; Must be at the end of RAM with appropriate buffer size
WOZMON_BUFFER    = $3F37     ; 200 bytes should be enough?? (3FFF - C8)