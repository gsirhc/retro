# retroweb — assemble and preview the deployed site.
#
#   make site        build _site/ : index.html at the root, the emulator at
#                    /altair8800/ (dev-only files stripped) — what CI publishes
#   make preview     stage _site/ (wasm + media first) and serve it on :8000 in
#                    the FOREGROUND — dies when the terminal closes or on Ctrl-C
#   make preview-bg  same, but detached: survives the terminal, logs to
#                    /tmp/retroweb-preview.log; stop it with `make preview-stop`
#   make preview-stop / preview-status   control / inspect the :8000 server
#   make preview-install    load a launchd agent (RunAtLoad + KeepAlive) that
#                    keeps :8000 up across logout, sleep and crashes; after
#                    editing the source run `make site` to refresh what it serves
#   make preview-uninstall  remove that agent
#   make clean       remove _site/

.PHONY: help site _stage preview preview-bg preview-stop preview-status \
        preview-install preview-uninstall clean

PREVIEW_PORT  ?= 8000
PREVIEW_PID   := /tmp/retroweb-preview.pid
PREVIEW_LOG   := /tmp/retroweb-preview.log
PREVIEW_LABEL := dev.retroweb.preview
PLIST         := $(HOME)/Library/LaunchAgents/$(PREVIEW_LABEL).plist
# system python is always present and new enough for `http.server -d/-b`
PY            := /usr/bin/python3

help:
	@sed -n 's/^#   //p' $(MAKEFILE_LIST)

# stage into a fresh dir, then swap it in atomically so a running preview
# server never finds its document root deleted out from under it
site:
	rm -rf _site.new
	mkdir -p _site.new/altair8800
	cp index.html _site.new/
	cp -R assets _site.new/ && rm -f _site.new/assets/README.md
	cp -R altair8800/web/. _site.new/altair8800/
	( cd _site.new/altair8800 && \
	  rm -rf tests node_modules coverage test-results playwright-report \
	         .playwright .v8-coverage && \
	  rm -f Makefile README.md devserve.py wasm_machine.cpp \
	        package.json package-lock.json playwright.config.ts )
	rm -rf _site && mv _site.new _site

# build the emulator + fetch media, then stage — what both preview modes need
_stage:
	$(MAKE) -C altair8800/web retro8080.js roms media
	$(MAKE) site

preview: _stage
	@if lsof -nP -iTCP:$(PREVIEW_PORT) -sTCP:LISTEN >/dev/null 2>&1; then \
	  echo; \
	  echo "  port $(PREVIEW_PORT) is already in use:"; \
	  echo "    make preview-stop        (a detached 'make preview-bg')"; \
	  echo "    make preview-uninstall   (the launchd agent)"; \
	  echo; exit 1; \
	fi
	@echo
	@echo "  http://0.0.0.0:$(PREVIEW_PORT)/             landing page"
	@echo "  http://0.0.0.0:$(PREVIEW_PORT)/altair8800/  the Altair emulator"
	@echo "  (foreground — leave this terminal open; Ctrl-C to stop)"
	@echo
	$(PY) -m http.server $(PREVIEW_PORT) -d _site -b 0.0.0.0

preview-bg: _stage
	@if lsof -nP -iTCP:$(PREVIEW_PORT) -sTCP:LISTEN >/dev/null 2>&1; then \
	  echo "  port $(PREVIEW_PORT) already in use — 'make preview-stop' first"; exit 1; \
	fi
	@nohup $(PY) -m http.server $(PREVIEW_PORT) -d _site -b 0.0.0.0 \
	  >$(PREVIEW_LOG) 2>&1 & echo $$! >$(PREVIEW_PID)
	@sleep 1; \
	if kill -0 $$(cat $(PREVIEW_PID)) 2>/dev/null; then \
	  echo "  serving _site on :$(PREVIEW_PORT) — pid $$(cat $(PREVIEW_PID)), log $(PREVIEW_LOG)"; \
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
preview-install: site
	@mkdir -p $(dir $(PLIST))
	@printf '%s\n' \
	  '<?xml version="1.0" encoding="UTF-8"?>' \
	  '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	  '<plist version="1.0"><dict>' \
	  '  <key>Label</key><string>$(PREVIEW_LABEL)</string>' \
	  '  <key>ProgramArguments</key><array>' \
	  '    <string>$(PY)</string><string>-m</string><string>http.server</string>' \
	  '    <string>$(PREVIEW_PORT)</string><string>-b</string><string>0.0.0.0</string>' \
	  '    <string>-d</string><string>$(CURDIR)/_site</string>' \
	  '  </array>' \
	  '  <key>RunAtLoad</key><true/>' \
	  '  <key>KeepAlive</key><true/>' \
	  '  <key>StandardOutPath</key><string>$(PREVIEW_LOG)</string>' \
	  '  <key>StandardErrorPath</key><string>$(PREVIEW_LOG)</string>' \
	  '</dict></plist>' >$(PLIST)
	@launchctl bootout gui/$$(id -u)/$(PREVIEW_LABEL) 2>/dev/null || true
	launchctl bootstrap gui/$$(id -u) $(PLIST)
	@echo "  agent loaded — http://<this-mac>.local:$(PREVIEW_PORT)/  (log: $(PREVIEW_LOG))"
	@echo "  refresh what it serves:  make -C retroweb site"

preview-uninstall:
	@launchctl bootout gui/$$(id -u)/$(PREVIEW_LABEL) 2>/dev/null || true
	@rm -f $(PLIST)
	@echo "  agent removed"

clean:
	rm -rf _site _site.new
