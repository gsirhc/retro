# retroweb — assemble and preview the deployed site.
#
#   make site        build _site/ : index.html at the root, the emulators at
#                    /altair8800/, /assembler6502/, /ibmpc-at/, /pacman/,
#                    /frogger/, /galaga/, /galaxian/, /scramble/, /pc486/
#                    (dev-only files stripped) — what CI publishes
#   make preview     stage _site/ (wasm + media first) and serve it over
#                    HTTPS on :8000 in the FOREGROUND — dies when the
#                    terminal closes or on Ctrl-C
#   make preview-bg  same, but detached: survives the terminal, logs to
#                    /tmp/retroweb-preview.log; stop it with `make preview-stop`
#   make preview-cert   generate/reuse the local TLS cert the preview server
#                    uses (auto-run by preview/preview-bg/preview-install) —
#                    see its own comment for why HTTPS, not just LAN reach
#   make preview-stop / preview-status   control / inspect the :8000 server
#   make preview-install    load a launchd agent (RunAtLoad + KeepAlive) that
#                    keeps :8000 up across logout, sleep and crashes; after
#                    editing the source run `make site` to refresh what it serves
#   make preview-uninstall  remove that agent
#   make clean       remove _site/
#   make lint        ESLint, HTMLHint, and cppcheck on this tree.
#                    JS/HTML need `npm ci` here (the lint target installs
#                    them if node_modules is missing). C++ needs cppcheck
#                    on PATH and gtest headers (libgtest-dev / googletest,
#                    or a native test build that already fetched them).

.PHONY: help site _stage preview preview-bg preview-stop preview-status \
        preview-install preview-uninstall preview-cert clean lint lint-js lint-html lint-cpp

PREVIEW_PORT  ?= 8000
PREVIEW_PID   := /tmp/retroweb-preview.pid
PREVIEW_LOG   := /tmp/retroweb-preview.log
PREVIEW_LABEL := dev.retroweb.preview
PLIST         := $(HOME)/Library/LaunchAgents/$(PREVIEW_LABEL).plist
# system python is always present and new enough for `http.server -d/-b`
PY            := /usr/bin/python3

PREVIEW_CERT_DIR := .preview-tls
PREVIEW_CERT      := $(PREVIEW_CERT_DIR)/cert.pem
PREVIEW_KEY       := $(PREVIEW_CERT_DIR)/key.pem

help:
	@sed -n 's/^#   //p' $(MAKEFILE_LIST)

# stage into a fresh dir, then swap it in atomically so a running preview
# server never finds its document root deleted out from under it
site:
	rm -rf _site.new
	mkdir -p _site.new/altair8800 _site.new/assembler6502 _site.new/ibmpc-at _site.new/pacman _site.new/frogger _site.new/galaga _site.new/galaxian _site.new/scramble _site.new/pc486
	cp index.html about.html _site.new/
	cp -R assets _site.new/ && rm -f _site.new/assets/README.md
	mkdir -p _site.new/shared
	cp shared/theme.css shared/pagebar.css shared/footer.css shared/footer.js _site.new/shared/
	cp -R altair8800/web/. _site.new/altair8800/
	( cd _site.new/altair8800 && \
	  rm -rf tests node_modules coverage test-results playwright-report \
	         .playwright .v8-coverage && \
	  rm -f Makefile README.md devserve.py wasm_machine.cpp \
	        package.json package-lock.json playwright.config.ts )
	cp -R assembler6502/web/. _site.new/assembler6502/
	( cd _site.new/assembler6502 && \
	  rm -rf tests node_modules coverage test-results playwright-report \
	         .playwright .v8-coverage && \
	  rm -f Makefile devserve.py wasm_machine.cpp gen_entrypoints.py \
	        gen_example_bin.cpp gen_example_bin \
	        package.json package-lock.json playwright.config.ts )
	cp -R ibmpc-at/web/. _site.new/ibmpc-at/
	( cd _site.new/ibmpc-at && \
	  rm -rf tests node_modules coverage test-results playwright-report \
	         .playwright .v8-coverage && \
	  rm -f Makefile devserve.py wasm_machine.cpp \
	        package.json package-lock.json playwright.config.ts )
	cp -R pacman/web/. _site.new/pacman/
	( cd _site.new/pacman && \
	  rm -rf tests node_modules coverage test-results playwright-report \
	         .playwright .v8-coverage && \
	  rm -f Makefile devserve.py wasm_machine.cpp \
	        package.json package-lock.json playwright.config.ts )
	cp -R frogger/web/. _site.new/frogger/
	( cd _site.new/frogger && \
	  rm -rf tests node_modules coverage test-results playwright-report \
	         .playwright .v8-coverage && \
	  rm -f Makefile devserve.py wasm_machine.cpp \
	        package.json package-lock.json playwright.config.ts )
	cp -R galaga/web/. _site.new/galaga/
	( cd _site.new/galaga && \
	  rm -rf tests node_modules coverage test-results playwright-report \
	         .playwright .v8-coverage && \
	  rm -f Makefile devserve.py wasm_machine.cpp \
	        package.json package-lock.json playwright.config.ts )
	cp -R galaxian/web/. _site.new/galaxian/
	( cd _site.new/galaxian && \
	  rm -rf tests node_modules coverage test-results playwright-report \
	         .playwright .v8-coverage && \
	  rm -f Makefile devserve.py wasm_machine.cpp \
	        package.json package-lock.json playwright.config.ts )
	cp -R scramble/web/. _site.new/scramble/
	( cd _site.new/scramble && \
	  rm -rf tests node_modules coverage test-results playwright-report \
	         .playwright .v8-coverage && \
	  rm -f Makefile devserve.py wasm_machine.cpp \
	        package.json package-lock.json playwright.config.ts )
	cp -R pc486/web/. _site.new/pc486/
	( cd _site.new/pc486 && \
	  rm -rf tests node_modules coverage test-results playwright-report \
	         .playwright .v8-coverage && \
	  rm -f Makefile devserve.py wasm_machine.cpp \
	        package.json package-lock.json playwright.config.ts )
	rm -rf _site && mv _site.new _site

# build the emulator + fetch media, then stage — what both preview modes need
_stage:
	$(MAKE) -C altair8800/web retro8080.js roms media shared
	$(MAKE) -C assembler6502/web wasm roms examples-bin shared
	$(MAKE) -C ibmpc-at/web ibmpcat.js roms hdd-image shared
	$(MAKE) -C pacman/web wasm roms shared
	$(MAKE) -C frogger/web wasm roms shared
	$(MAKE) -C galaga/web wasm roms shared
	$(MAKE) -C galaxian/web wasm roms shared
	$(MAKE) -C scramble/web wasm roms shared
	$(MAKE) -C pc486/web pc486.js roms hdd-image cdrom-image shared
	$(MAKE) site

# Locally-trusted TLS cert for the preview server, covering localhost, this
# Mac's LAN mDNS name, and its current LAN IP. The deployed site is always
# https://, but a browser only treats plain http:// as a "secure context"
# on localhost itself -- not a LAN address, even on your own network -- and
# disables secure-context-gated APIs there (e.g. Chrome's AudioWorklet;
# see pc486/PC486_REVIEW.md, no sound over a plain-http LAN preview).
# Serving the preview over https:// fixes that for every machine at once.
# Uses mkcert (https://github.com/FiloSottile/mkcert) if installed, so a
# Mac that's run `mkcert -install` trusts it with no browser warning at
# all -- to get that same no-warning trust on another device (e.g. a
# MacBook previewing this Mac's LAN address), copy `mkcert -CAROOT`'s
# rootCA.pem over and run `mkcert -install` there too. Falls back to a
# plain openssl self-signed cert if mkcert isn't installed, which still
# satisfies "secure context" but needs a one-time per-browser "proceed
# anyway" click through its warning. Regenerate after a LAN IP change with
# `rm -rf $(PREVIEW_CERT_DIR) && make preview-cert`.
preview-cert:
	@mkdir -p $(PREVIEW_CERT_DIR); \
	if [ -f $(PREVIEW_CERT) ] && [ -f $(PREVIEW_KEY) ]; then \
	  echo "  cert already exists: $(PREVIEW_CERT) (rm -rf $(PREVIEW_CERT_DIR) to regenerate, e.g. after a LAN IP change)"; \
	  exit 0; \
	fi; \
	host=$$(hostname -s 2>/dev/null || echo localhost); \
	 lanip=""; \
	 for i in en0 en1 en2 en3 en4; do \
	   ip=$$(ipconfig getifaddr $$i 2>/dev/null); [ -n "$$ip" ] && lanip=$$ip && break; \
	 done; \
	 names="localhost 127.0.0.1 ::1 $$host.local"; \
	 [ -n "$$lanip" ] && names="$$names $$lanip"; \
	 if command -v mkcert >/dev/null 2>&1; then \
	   echo "  generating a locally-trusted cert via mkcert for: $$names"; \
	   mkcert -cert-file $(PREVIEW_CERT) -key-file $(PREVIEW_KEY) $$names; \
	 else \
	   echo "  mkcert not found -- falling back to a self-signed cert (needs a one-time browser warning click-through)"; \
	   san="DNS:localhost,DNS:$$host.local,IP:127.0.0.1"; \
	   [ -n "$$lanip" ] && san="$$san,IP:$$lanip"; \
	   openssl req -x509 -newkey rsa:2048 -nodes -days 825 \
	     -keyout $(PREVIEW_KEY) -out $(PREVIEW_CERT) \
	     -subj "/CN=$$host" -addext "subjectAltName=$$san"; \
	 fi

preview: _stage preview-cert
	@if lsof -nP -iTCP:$(PREVIEW_PORT) -sTCP:LISTEN >/dev/null 2>&1; then \
	  echo; \
	  echo "  port $(PREVIEW_PORT) is already in use:"; \
	  echo "    make preview-stop        (a detached 'make preview-bg')"; \
	  echo "    make preview-uninstall   (the launchd agent)"; \
	  echo; exit 1; \
	fi
	@echo
	@echo "  https://0.0.0.0:$(PREVIEW_PORT)/             landing page"
	@echo "  https://0.0.0.0:$(PREVIEW_PORT)/altair8800/  the Altair emulator"
	@echo "  https://0.0.0.0:$(PREVIEW_PORT)/assembler6502/  the 6502 Assembler emulator"
	@echo "  https://0.0.0.0:$(PREVIEW_PORT)/ibmpc-at/    the IBM PC/AT emulator"
	@echo "  https://0.0.0.0:$(PREVIEW_PORT)/pacman/      the Pac-Man arcade emulator"
	@echo "  https://0.0.0.0:$(PREVIEW_PORT)/frogger/     the Frogger arcade emulator"
	@echo "  https://0.0.0.0:$(PREVIEW_PORT)/galaga/      the Galaga arcade emulator"
	@echo "  https://0.0.0.0:$(PREVIEW_PORT)/galaxian/    the Galaxian arcade emulator"
	@echo "  https://0.0.0.0:$(PREVIEW_PORT)/scramble/    the Scramble arcade emulator"
	@echo "  https://0.0.0.0:$(PREVIEW_PORT)/pc486/       the 486DX2-66 emulator"
	@echo "  (foreground — leave this terminal open; Ctrl-C to stop)"
	@echo
	$(PY) serve_nocache.py $(PREVIEW_PORT) _site 0.0.0.0 $(PREVIEW_CERT) $(PREVIEW_KEY)

preview-bg: _stage preview-cert
	@if lsof -nP -iTCP:$(PREVIEW_PORT) -sTCP:LISTEN >/dev/null 2>&1; then \
	  echo "  port $(PREVIEW_PORT) already in use — 'make preview-stop' first"; exit 1; \
	fi
	@nohup $(PY) serve_nocache.py $(PREVIEW_PORT) _site 0.0.0.0 $(PREVIEW_CERT) $(PREVIEW_KEY) \
	  >$(PREVIEW_LOG) 2>&1 & echo $$! >$(PREVIEW_PID)
	@sleep 1; \
	if kill -0 $$(cat $(PREVIEW_PID)) 2>/dev/null; then \
	  echo "  serving _site on https://:$(PREVIEW_PORT) — pid $$(cat $(PREVIEW_PID)), log $(PREVIEW_LOG)"; \
	  echo "  survives closing this terminal; gone on reboot. stop: make preview-stop"; \
	else echo "  failed to start — see $(PREVIEW_LOG)"; rm -f $(PREVIEW_PID); exit 1; fi

preview-stop:
	@if [ -f $(PREVIEW_PID) ] && kill $$(cat $(PREVIEW_PID)) 2>/dev/null; then \
	  echo "  stopped (pid $$(cat $(PREVIEW_PID)))"; \
	else echo "  no detached preview server running (launchd agent, if any, is separate)"; fi
	@rm -f $(PREVIEW_PID)

preview-status:
	@lsof -nP -iTCP:$(PREVIEW_PORT) -sTCP:LISTEN 2>/dev/null \
	  || echo "  nothing listening on :$(PREVIEW_PORT)"

# launchd LaunchAgent: RunAtLoad + KeepAlive, so :8000 comes back after logout,
# sleep, a crash, or a bad request. Serves $(CURDIR)/_site — rebuild it with
# `make site` after changing the source.
preview-install: site preview-cert
	@mkdir -p $(dir $(PLIST))
	@printf '%s\n' \
	  '<?xml version="1.0" encoding="UTF-8"?>' \
	  '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	  '<plist version="1.0"><dict>' \
	  '  <key>Label</key><string>$(PREVIEW_LABEL)</string>' \
	  '  <key>ProgramArguments</key><array>' \
	  '    <string>$(PY)</string><string>$(CURDIR)/serve_nocache.py</string>' \
	  '    <string>$(PREVIEW_PORT)</string><string>$(CURDIR)/_site</string><string>0.0.0.0</string>' \
	  '    <string>$(CURDIR)/$(PREVIEW_CERT)</string><string>$(CURDIR)/$(PREVIEW_KEY)</string>' \
	  '  </array>' \
	  '  <key>RunAtLoad</key><true/>' \
	  '  <key>KeepAlive</key><true/>' \
	  '  <key>StandardOutPath</key><string>$(PREVIEW_LOG)</string>' \
	  '  <key>StandardErrorPath</key><string>$(PREVIEW_LOG)</string>' \
	  '</dict></plist>' >$(PLIST)
	@launchctl bootout gui/$$(id -u)/$(PREVIEW_LABEL) 2>/dev/null || true
	launchctl bootstrap gui/$$(id -u) $(PLIST)
	@echo "  agent loaded — https://<this-mac>.local:$(PREVIEW_PORT)/  (log: $(PREVIEW_LOG))"
	@echo "  refresh what it serves:  make -C retroweb site"

preview-uninstall:
	@launchctl bootout gui/$$(id -u)/$(PREVIEW_LABEL) 2>/dev/null || true
	@rm -f $(PLIST)
	@echo "  agent removed"

clean:
	rm -rf _site _site.new

# Source only: not _site, vendored xterm, copied web/shared, or test build trees.
lint: lint-js lint-html lint-cpp

lint-js:
	@test -x node_modules/.bin/eslint || npm ci
	npx eslint .

lint-html:
	@test -x node_modules/.bin/htmlhint || npm ci
	npm run lint:html

lint-cpp:
	sh lint-cpp.sh
