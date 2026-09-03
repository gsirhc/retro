# retroweb — assemble and preview the deployed site.
#
#   make site      build _site/ : index.html at the root, the emulator at
#                  /altair8800/ (dev-only files stripped) — what CI publishes
#   make preview   build the emulator's wasm + fetch media, stage _site/,
#                  and serve it on :8000 (LAN) exactly as deployed
#   make clean     remove _site/

.PHONY: help site preview clean

PREVIEW_PORT ?= 8000

help:
	@sed -n 's/^#   //p' $(MAKEFILE_LIST)

# stage into a fresh dir, then swap it in atomically so a running preview
# server never finds its document root deleted out from under it
site:
	rm -rf _site.new
	mkdir -p _site.new/altair8800
	cp index.html _site.new/
	cp -R altair8800/web/. _site.new/altair8800/
	( cd _site.new/altair8800 && \
	  rm -rf tests node_modules coverage test-results playwright-report \
	         .playwright .v8-coverage && \
	  rm -f Makefile README.md devserve.py wasm_machine.cpp \
	        package.json package-lock.json playwright.config.ts )
	rm -rf _site && mv _site.new _site

preview:
	$(MAKE) -C altair8800/web retro8080.js roms media
	$(MAKE) site
	@if lsof -nP -iTCP:$(PREVIEW_PORT) -sTCP:LISTEN >/dev/null 2>&1; then \
	  echo; \
	  echo "  port $(PREVIEW_PORT) is already in use (a stale preview server?)."; \
	  echo "  stop it and retry:   lsof -ti tcp:$(PREVIEW_PORT) | xargs kill"; \
	  echo; exit 1; \
	fi
	@echo
	@echo "  http://0.0.0.0:$(PREVIEW_PORT)/             landing page"
	@echo "  http://0.0.0.0:$(PREVIEW_PORT)/altair8800/  the Altair emulator"
	@echo
	python3 -m http.server $(PREVIEW_PORT) -d _site -b 0.0.0.0

clean:
	rm -rf _site _site.new
