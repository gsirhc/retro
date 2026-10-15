CORE = ..
SHARED = ../../shared
CPU = ../../shared/cpu
ATARI = ../../shared/atari
NAME = asteroids
EXPORT_NAME = AsteroidsArcade
SRC = wasm_machine.cpp $(CORE)/machine.cpp $(CPU)/cpu_mos6502.cpp $(ATARI)/dvg.cpp
HDR = wasm_machine.cpp $(CORE)/machine.h $(CPU)/cpu_mos6502.h $(ATARI)/dvg.h hwtest_roms.h
INCLUDES = -I$(CPU) -I$(ATARI)
ROM_ARGS = --png ../../assets/asteroids-cabinet.png

include $(SHARED)/arcade.mk
