CORE = ..
SHARED = ../../shared
Z80 = ../../shared/cpu
NAME = galaga
EXPORT_NAME = GalagaArcade
SRC = wasm_machine.cpp $(CORE)/machine.cpp $(CORE)/video.cpp $(CORE)/mb88.cpp \
      $(Z80)/cpu_z80.cpp $(SHARED)/namco/wsg.cpp
HDR = wasm_machine.cpp $(CORE)/machine.h $(CORE)/video.h $(CORE)/mb88.h \
      $(Z80)/cpu_z80.h $(SHARED)/namco/wsg.h hwtest_roms.h
INCLUDES = -I$(Z80)
ROM_ARGS = --png ../../assets/galaga-cabinet.png

include $(SHARED)/arcade.mk
