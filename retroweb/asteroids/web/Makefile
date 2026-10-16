CORE = ..
SHARED = ../../shared
CPU = ../../shared/cpu
ATARI = ../../shared/atari
NAME = asteroids
EXPORT_NAME = AsteroidsArcade
SRC = wasm_machine.cpp $(CORE)/machine.cpp $(CPU)/cpu_mos6502.cpp $(ATARI)/dvg.cpp
INCLUDES = -I$(CPU) -I$(ATARI)
ROM_ARGS = --png ../../assets/asteroids-cabinet.png

include $(SHARED)/arcade.mk
