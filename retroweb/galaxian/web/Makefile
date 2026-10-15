CORE = ..
SHARED = ../../shared
Z80 = ../../shared/cpu
GAL = $(SHARED)/galaxian
NAME = galaxian
EXPORT_NAME = GalaxianArcade
SRC = wasm_machine.cpp $(CORE)/machine.cpp $(CORE)/sound.cpp \
      $(Z80)/cpu_z80.cpp $(GAL)/video.cpp
HDR = wasm_machine.cpp $(CORE)/machine.h $(CORE)/sound.h $(Z80)/cpu_z80.h \
      $(GAL)/video.h hwtest_roms.h
INCLUDES = -I$(Z80)
ROM_ARGS = --png ../../assets/galaxian-cabinet.png

include $(SHARED)/arcade.mk
