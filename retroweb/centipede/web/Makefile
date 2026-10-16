CORE = ..
SHARED = ../../shared
CPU = ../../shared/cpu
ATARI = ../../shared/atari
NAME = centipede
EXPORT_NAME = CentipedeArcade
SRC = wasm_machine.cpp $(CORE)/machine.cpp $(CPU)/cpu_mos6502.cpp $(ATARI)/pokey.cpp
INCLUDES = -I$(CPU) -I$(ATARI)
ROM_ARGS = --png ../../assets/centipede-cabinet.png

include $(SHARED)/arcade.mk
