CORE = ..
SHARED = ../../shared
CPU = ../../shared/cpu
ATARI = ../../shared/atari
NAME = missile
EXPORT_NAME = MissileArcade
SRC = wasm_machine.cpp $(CORE)/machine.cpp $(CPU)/cpu_mos6502.cpp $(ATARI)/pokey.cpp
INCLUDES = -I$(CPU) -I$(ATARI)

include $(SHARED)/arcade.mk
