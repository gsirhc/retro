# retroweb, assemble and preview the deployed site.
#
#   make site        build _site/ : index.html + about.html + arcade/ at the
#                    root, the emulators at
#                    /altair8800/, /assembler6502/, /ibmpc-at/, /pacman/,
#                    /frogger/, /galaga/, /galaxian/, /scramble/, /asteroids/,
#                    /centipede/, /missile/, /pc486/ (dev-only files stripped), what CI publishes
#   make preview     stage _site/ (wasm + media first) and serve it over
#                    HTTPS on :8000 in the FOREGROUND, dies when the
#                    terminal closes or on Ctrl-C
#   make preview-bg  same, but detached: survives the terminal, logs to
#                    /tmp/retroweb-preview.log; stop it with `make preview-stop`
#   make watch       like preview, but rebuilds/restages on HTML/CSS/JS/C++
#                    edits and live-reloads open tabs (optional MACHINE=pc486)
#   make watch-bg    detached watch; stop with `make preview-stop`
#   make preview-cert   generate/reuse the local TLS cert for the preview
#                    server (auto-run by preview/preview-bg/preview-install)
#   make preview-stop / preview-status   control / inspect the :8000 server
#   make preview-install    load a launchd agent (RunAtLoad + KeepAlive) that
#                    keeps :8000 up across logout, sleep and crashes; after
#                    editing the source run `make site` to refresh what it serves
#                    (or use `make watch` / `make watch-bg` instead)
#   make preview-uninstall  remove that agent
#   make clean       remove _site/
#   make lint        ESLint, HTMLHint, tsc, cppcheck, and actionlint.
#                    JS/HTML need `npm ci` here (the lint target installs
#                    them if node_modules is missing). C++ needs cppcheck
#                    on PATH and gtest headers (libgtest-dev / googletest,
#                    or a native test build that already fetched them).

.PHONY: help site _stage preview preview-bg watch watch-bg preview-stop preview-status \
        preview-install preview-uninstall preview-cert clean lint lint-js lint-html lint-ts lint-cpp lint-staging lint-actions

PREVIEW_PORT  ?= 8000
PREVIEW_PID   := /tmp/retroweb-preview.pid
PREVIEW_LOG   := /tmp/retroweb-preview.log
PREVIEW_LABEL := dev.retroweb.preview
PLIST         := $(HOME)/Library/LaunchAgents/$(PREVIEW_LABEL).plist
# system python is always present and new enough for `http.server -d/-b`
PY            := /usr/bin/python3
# Optional: MACHINE=pc486 make watch, limit wasm rebuilds to one emulator
WATCH_ARGS    := --watch $(if $(MACHINE),--machine=$(MACHINE),)

PREVIEW_CERT_DIR := .preview-tls
PREVIEW_CERT      := $(PREVIEW_CERT_DIR)/cert.pem
PREVIEW_KEY       := $(PREVIEW_CERT_DIR)/key.pem

help:
	@sed -n 's/^#   //p' $(MAKEFILE_LIST)

# stage into a fresh dir, then swap it in atomically so a running preview
# server never finds its document root deleted out from under it
MACHINES = altair8800 assembler6502 ibmpc-at pacman frogger galaga galaxian scramble asteroids centipede missile pc486
SITE ?= _site

site:
	rm -rf $(SITE).new
	mkdir -p $(SITE).new/arcade $(SITE).new/shared
	cp index.html about.html $(SITE).new/
	cp arcade/index.html $(SITE).new/arcade/
	cp -Rp assets $(SITE).new/ && rm -f $(SITE).new/assets/README.md
	find $(SITE).new -name .DS_Store -delete
	cp shared/theme.css shared/pagebar.css shared/footer.css shared/footer.js \
	   shared/theme-picker.js shared/winbtns.js shared/thumb-art.js $(SITE).new/shared/
	for m in $(MACHINES); do \
	  $(MAKE) --no-print-directory -C $$m/web stage DEST=$(abspath $(SITE).new)/$$m || exit 1; \
	done
	python3 check_site.py $(SITE).new
	rm -rf $(SITE) && mv $(SITE).new $(SITE)

# build the emulator + fetch media, then stage, what both preview modes need
_stage:
	$(MAKE) -C altair8800/web retro8080.js roms media shared
	$(MAKE) -C assembler6502/web wasm roms examples-bin shared
	$(MAKE) -C ibmpc-at/web ibmpcat.js roms hdd-image library-images preset-images shared
	$(MAKE) -C pacman/web wasm roms shared
	$(MAKE) -C frogger/web wasm roms shared
	$(MAKE) -C galaga/web wasm roms shared
	$(MAKE) -C galaxian/web wasm roms shared
	$(MAKE) -C scramble/web wasm roms shared
	$(MAKE) -C asteroids/web wasm roms shared
	$(MAKE) -C centipede/web wasm roms shared
	$(MAKE) -C missile/web wasm roms shared
	$(MAKE) -C pc486/web pc486.js pc486-perf.js roms hdd-image driver-images library-images preset-images shared
	$(MAKE) site

# Local TLS cert (localhost, LAN name, LAN IP). Plain http:// on a LAN address
# is an insecure context and Chrome disables AudioWorklet there. Uses mkcert if
# installed, else self-signed openssl. Regenerate after a LAN IP change with
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
	@echo "  https://0.0.0.0:$(PREVIEW_PORT)/asteroids/   the Asteroids arcade emulator"
	@echo "  https://0.0.0.0:$(PREVIEW_PORT)/centipede/   the Centipede arcade emulator"
	@echo "  https://0.0.0.0:$(PREVIEW_PORT)/missile/     the Missile Command arcade emulator"
	@echo "  https://0.0.0.0:$(PREVIEW_PORT)/pc486/       the 486 DX2-66 emulator"
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

# Like preview, but polls HTML/CSS/JS/C++ under retroweb/, rebuilds wasm when
# needed, re-runs `make site`, and live-reloads open tabs. MACHINE=name limits
# wasm rebuilds to one emulator (root + shared front-end still restage).
watch: _stage preview-cert
	@if lsof -nP -iTCP:$(PREVIEW_PORT) -sTCP:LISTEN >/dev/null 2>&1; then \
	  echo; \
	  echo "  port $(PREVIEW_PORT) is already in use:"; \
	  echo "    make preview-stop        (a detached 'make preview-bg' / 'make watch-bg')"; \
	  echo "    make preview-uninstall   (the launchd agent)"; \
	  echo; exit 1; \
	fi
	@echo
	@echo "  https://0.0.0.0:$(PREVIEW_PORT)/             landing page (watch + live-reload)"
	@echo "  edit HTML/CSS/JS/C++ under retroweb/ — restages _site/ and reloads tabs"
	@if [ -n "$(MACHINE)" ]; then echo "  MACHINE=$(MACHINE) — wasm rebuilds only for that emulator"; fi
	@echo "  (foreground — leave this terminal open; Ctrl-C to stop)"
	@echo
	$(PY) serve_nocache.py $(PREVIEW_PORT) _site 0.0.0.0 $(PREVIEW_CERT) $(PREVIEW_KEY) $(WATCH_ARGS)

watch-bg: _stage preview-cert
	@if lsof -nP -iTCP:$(PREVIEW_PORT) -sTCP:LISTEN >/dev/null 2>&1; then \
	  echo "  port $(PREVIEW_PORT) already in use — 'make preview-stop' first"; exit 1; \
	fi
	@nohup $(PY) serve_nocache.py $(PREVIEW_PORT) _site 0.0.0.0 $(PREVIEW_CERT) $(PREVIEW_KEY) $(WATCH_ARGS) \
	  >$(PREVIEW_LOG) 2>&1 & echo $$! >$(PREVIEW_PID)
	@sleep 1; \
	if kill -0 $$(cat $(PREVIEW_PID)) 2>/dev/null; then \
	  echo "  watching + serving _site on https://:$(PREVIEW_PORT) — pid $$(cat $(PREVIEW_PID)), log $(PREVIEW_LOG)"; \
	  if [ -n "$(MACHINE)" ]; then echo "  MACHINE=$(MACHINE)"; fi; \
	  echo "  stop: make preview-stop"; \
	else echo "  failed to start — see $(PREVIEW_LOG)"; rm -f $(PREVIEW_PID); exit 1; fi

preview-stop:
	@if [ -f $(PREVIEW_PID) ] && kill $$(cat $(PREVIEW_PID)) 2>/dev/null; then \
	  echo "  stopped (pid $$(cat $(PREVIEW_PID)))"; \
	else echo "  no detached preview server running (launchd agent, if any, is separate)"; fi
	@rm -f $(PREVIEW_PID)

preview-status:
	@lsof -nP -iTCP:$(PREVIEW_PORT) -sTCP:LISTEN 2>/dev/null \
	  || echo "  nothing listening on :$(PREVIEW_PORT)"

# launchd agent: RunAtLoad + KeepAlive. Serves $(CURDIR)/_site.
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
lint: lint-js lint-html lint-ts lint-cpp lint-staging lint-actions

lint-ts:
	@test -x node_modules/.bin/tsc || npm ci
	npx tsc -p tsconfig.json

lint-staging:
	sh -n stage_web.sh
	python3 check_site.py --selftest
	python3 strip_fast_flag.py --check ibmpc-at/web/app.js pc486/web/app.js

lint-js:
	@test -x node_modules/.bin/eslint || npm ci
	npx eslint .

lint-html:
	@test -x node_modules/.bin/htmlhint || npm ci
	npm run lint:html

lint-cpp:
	sh lint-cpp.sh

lint-actions:
	@sh fetch-actionlint.sh >/dev/null
	cd .. && retroweb/.tools/actionlint-1.7.12/actionlint
