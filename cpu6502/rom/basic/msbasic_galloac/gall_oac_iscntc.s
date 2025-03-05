ISCNTC:
    JSR MONRDKEY
    BCC not_cntc
    CMP #3                ; CTRL-C ASCII code
    BNE not_cntc
    JMP is_cntc

not_cntc:
    RTS

is_cntc:
    ; fall through