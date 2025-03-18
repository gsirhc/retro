; configuration
CONFIG_2A := 1

CONFIG_SCRTCH_ORDER := 2

; zero page
ZP_START0 = $00   ; SEE bios.s
ZP_START1 = $18   ; 9 bytes
ZP_START2 = $21   ; 6 bytes + BASIC INPUTBUFFER (leave room)
ZP_START3 = $66   ; 11  bytes
ZP_START4 = $77   ; ALOT - leave room

;
;; extra/override ZP variables
USR	:= GORESTART

;; constants
SPACE_FOR_GOSUB := $3E
STACK_TOP := $FA
WIDTH := 80
WIDTH2 := 80

RAMSTART2 := $0400

;; monitor functions  defined in bios.s
;LOAD	:= $FFD5
;SAVE	:= $FFD8
;ISCNTC	:= $FFE1
;MONCOUT := $FFD2
;MONRDKEY := $FFE4
