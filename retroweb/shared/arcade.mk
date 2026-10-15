# web/Makefile body for the arcade cabinets. Set CORE, SHARED, NAME, EXPORT_NAME,
# SRC, HDR, INCLUDES and ROM_ARGS, then include this. Needs Emscripten on PATH.
EMCC ?= em++
EMFLAGS = -O2 -std=c++17 -lembind --no-entry \
  -sMODULARIZE=1 -sEXPORT_NAME=$(EXPORT_NAME) \
  -sENVIRONMENT=web \
  -sALLOW_MEMORY_GROWTH=1 \
  -sFILESYSTEM=0 \
  -sSTACK_SIZE=1MB

$(NAME).js: $(SRC) $(HDR)
	$(EMCC) $(EMFLAGS) -I. -I$(CORE) $(INCLUDES) -I$(SHARED) $(SRC) -o $@

hwtest_roms.h: $(CORE)/roms/hwtest/gen_hwtest.py
	python3 $(CORE)/roms/hwtest/gen_hwtest.py --dir roms --header hwtest_roms.h

.PHONY: roms wasm serve test test-smoke clean
wasm: $(NAME).js
roms: hwtest_roms.h
	python3 $(CORE)/roms/hwtest/gen_hwtest.py --dir roms --header hwtest_roms.h $(ROM_ARGS)

serve: $(NAME).js roms shared
	@echo "serving on http://localhost:8000  (Ctrl-C to stop)"
	python3 devserve.py 8000

test: $(NAME).js roms shared
	npx playwright test

# Smoke: tests/smoke.spec.ts only (deploy CI). Full suite is `make test`.
test-smoke: $(NAME).js roms shared
	npx playwright test tests/smoke.spec.ts

clean:
	rm -f $(NAME).js $(NAME).wasm hwtest_roms.h
	rm -rf roms shared
	rm -rf test-results playwright-report coverage .v8-coverage

SHARED_EXTRA = $(SHARED_ARCADE)
STAGE = index.html app.js $(NAME).js $(NAME).wasm assets roms shared

include $(SHARED)/web.mk
