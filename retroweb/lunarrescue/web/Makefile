CORE = ..
SHARED = ../../shared
CPU = ../../shared/cpu
MW = ../../shared/mw8080
NAME = lunarrescue
EXPORT_NAME = LunarRescueArcade
SRC = wasm_machine.cpp $(CORE)/machine.cpp $(MW)/board.cpp $(MW)/sound.cpp $(CPU)/cpu_i8080.cpp
INCLUDES = -I$(CPU)
ROM_ARGS = --png ../../assets/lunarrescue-cabinet.png

include $(SHARED)/arcade.mk
