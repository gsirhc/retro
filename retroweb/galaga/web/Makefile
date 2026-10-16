CORE = ..
SHARED = ../../shared
Z80 = ../../shared/cpu
NAME = galaga
EXPORT_NAME = GalagaArcade
SRC = wasm_machine.cpp $(CORE)/machine.cpp $(CORE)/video.cpp $(CORE)/mb88.cpp \
      $(Z80)/cpu_z80.cpp $(SHARED)/namco/wsg.cpp
INCLUDES = -I$(Z80)
ROM_ARGS = --png ../../assets/galaga-cabinet.png

include $(SHARED)/arcade.mk
