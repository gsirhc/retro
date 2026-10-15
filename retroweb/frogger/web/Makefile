CORE = ..
SHARED = ../../shared
Z80 = ../../shared/cpu
GAL = $(SHARED)/galaxian
NAME = frogger
EXPORT_NAME = FroggerArcade
SRC = wasm_machine.cpp $(CORE)/machine.cpp $(Z80)/cpu_z80.cpp \
      $(GAL)/video.cpp $(GAL)/ay8910.cpp $(GAL)/konami_sound.cpp $(GAL)/i8255.cpp $(GAL)/timer.cpp
HDR = wasm_machine.cpp $(CORE)/machine.h $(Z80)/cpu_z80.h \
      $(CORE)/video.h $(CORE)/ay8910.h $(CORE)/i8255.h \
      $(GAL)/video.h $(GAL)/ay8910.h $(GAL)/konami_sound.h $(GAL)/i8255.h $(GAL)/timer.h hwtest_roms.h
INCLUDES = -I$(Z80)
ROM_ARGS = --png ../../assets/frogger-cabinet.png

include $(SHARED)/arcade.mk
