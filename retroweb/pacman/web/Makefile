CORE = ..
SHARED = ../../shared
Z80 = ../../shared/cpu
NAME = pacman
EXPORT_NAME = PacmanArcade
SRC = wasm_machine.cpp $(CORE)/machine.cpp $(Z80)/cpu_z80.cpp $(CORE)/video.cpp $(SHARED)/namco/wsg.cpp
HDR = wasm_machine.cpp $(CORE)/machine.h $(Z80)/cpu_z80.h $(CORE)/video.h $(CORE)/wsg.h \
      $(SHARED)/namco/wsg.h hwtest_roms.h
INCLUDES = -I$(Z80)
ROM_ARGS = --png ../../assets/pacman-cabinet.png --png-ms ../../assets/mspacman-cabinet.png

include $(SHARED)/arcade.mk

coverage: pacman.js roms shared
	COVERAGE=1 npx playwright test
