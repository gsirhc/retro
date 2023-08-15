ACIA_DATA = $5000
ACIA_STATUS = $5001
ACIA_CMD = $5002
ACIA_CRTL = $5003

  .org $8000

reset:
  ldx #ff
  txs
  
  lda #0
  lda ACIA_STATUS
  