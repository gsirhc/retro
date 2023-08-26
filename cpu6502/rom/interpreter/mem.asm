; OS control memory addresses
UNPROCESS_CHARS  = $0000
COMMAND          = $0000 + 1 
PROGRAM_COUNTER  = $0000 + 2
START_MEM        = $0000 + 3 ; Always last as the program is stored from here on

; COMMAND values
LIST_CMD  = %00000001
RUN_CMD   = %00000010
NO_CMD    = %11111111
