CORE = ..
SHARED = ../../shared
CPU = ../../shared/cpu
ATARI = ../../shared/atari
NAME = battlezone
EXPORT_NAME = BattlezoneArcade
SRC = wasm_machine.cpp $(CORE)/machine.cpp $(CORE)/sound.cpp $(CPU)/cpu_mos6502.cpp \
  $(ATARI)/avg.cpp $(ATARI)/mathbox.cpp $(ATARI)/pokey.cpp
INCLUDES = -I$(CPU) -I$(ATARI)

include $(SHARED)/arcade.mk
