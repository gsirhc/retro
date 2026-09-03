# retroweb — assemble and preview the deployed site.
#
#   make site      build _site/ : index.html at the root, the emulator at
#                  /altair8800/ (dev-only files stripped) — what CI publishes
#   make preview   build the emulator's wasm + fetch media, stage _site/,
#                  and serve it on :8000 (LAN) exactly as deployed
#   make clean     remove _site/

.PHONY: help site preview clean

help:
	@sed -n 's/^#   //p' $(MAKEFILE_LIST)

site:
	rm -rf _site
	mkdir -p _site/altair8800
	cp index.html _site/
	cp -R altair8800/web/. _site/altair8800/
	( cd _site/altair8800 && \
	  rm -rf tests node_modules coverage test-results playwright-report \
	         .playwright .v8-coverage && \
	  rm -f Makefile README.md devserve.py wasm_machine.cpp \
	        package.json package-lock.json playwright.config.ts )

preview:
	$(MAKE) -C altair8800/web retro8080.js roms media
	$(MAKE) site
	@echo
	@echo "  http://0.0.0.0:8000/             landing page"
	@echo "  http://0.0.0.0:8000/altair8800/  the Altair emulator"
	@echo
	python3 -m http.server 8000 -d _site -b 0.0.0.0

clean:
	rm -rf _site
