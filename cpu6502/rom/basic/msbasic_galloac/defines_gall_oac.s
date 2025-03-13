; configuration
CONFIG_2A := 1

CONFIG_SCRTCH_ORDER := 2

; zero page
ZP_START0 = $00
ZP_START1 = $04
ZP_START2 = $0E
ZP_START3 = $64
ZP_START4 = $6F
;
;; extra/override ZP variables
USR	:= GORESTART

;; constants
SPACE_FOR_GOSUB := $3E
STACK_TOP := $FA
WIDTH := 40
WIDTH2 := 30

RAMSTART2 := $0400

;; monitor functions  defined in bios.s
;LOAD	:= $FFD5
;SAVE	:= $FFD8
;ISCNTC	:= $FFE1
;MONCOUT := $FFD2
;MONRDKEY := $FFE4
