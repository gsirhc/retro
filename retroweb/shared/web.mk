# Targets every machine's web/Makefile shares. Include it last, after STAGE and SHARED_EXTRA.
SHARED ?= ../../shared
SHARED_BASE = theme.css theme-init.js theme-picker.js pagebar.css \
  fullscreen.css fullscreen.js focus-hint.css focus-hint.js \
  footer.css footer.js winbtns.js
SHARED_PC = pc-keyboard.css pc-keyboard.js disk-drop.css disk-drop.js \
  disk-library.css disk-library.js hdd-worker.js hdd-convert.js
SHARED_ARCADE = arcade-cab.css arcade-touch.js arcade-core.js \
  cab-start-1p-yellow.png cab-start-2p-yellow.png \
  cab-start-1p-white.png cab-start-2p-white.png

.PHONY: shared test-install stage

# Header dependencies of a wasm module, written by a preprocessor pass after each build.
write_deps = $(EMCC) -std=c++17 -MM -MP -MT $@ $(1) > $(basename $@).d
-include $(wildcard *.d)

# Copied from retroweb/shared/ at build time; only web/ gets staged.
shared:
	mkdir -p shared
	cp -R $(addprefix $(SHARED)/,$(SHARED_BASE) $(SHARED_EXTRA)) shared/
ifneq ($(SHARED_TESTS),)
	rm -rf tests/shared
	mkdir -p tests/shared
	cp $(SHARED_TESTS) tests/shared/
endif

test-install:
	npm ci
	npx playwright install --with-deps chromium

# What the deployed site serves; `make -C retroweb site` calls this with DEST.
stage:
	../../stage_web.sh $(DEST) $(STAGE)
	$(STAGE_POST)
