CORE = ..
SHARED = ../../shared
CPU = ../../shared/cpu
NAME = invaders
EXPORT_NAME = InvadersArcade
SRC = wasm_machine.cpp $(CORE)/machine.cpp $(CORE)/sound.cpp $(CPU)/cpu_i8080.cpp
INCLUDES = -I$(CPU)
ROM_ARGS = --png ../../assets/invaders-cabinet.png

include $(SHARED)/arcade.mk
