# retroweb

The browser emulators and the landing page that lists them.

- [`index.html`](index.html) — the landing page (site root). Plain static HTML
  plus one image from [`assets/`](assets/) (a screenshot of the emulator's own
  rendered front panel); a `<select id="pageTheme">` that shares the
  `retro8080.theme` `localStorage` key with the emulators, so a theme choice
  carries across.
- [`altair8800/`](altair8800/) — MITS Altair 8800 (Intel 8080). C++ core +
  GoogleTest + WebAssembly front end in `altair8800/web/`. Deploys to
  `/altair8800/`. See its [README](altair8800/README.md).

## The deployed site

CI builds the front end, fetches the pinned Altair BASIC / CP/M media, then
**stages** the site so URLs are clean:

```
_site/index.html      <- retroweb/index.html
_site/assets/         <- retroweb/assets/
_site/altair8800/     <- retroweb/altair8800/web/  (dev-only files stripped)
```

`_site/` is git-ignored, built by `make -C retroweb site` (CI runs the same
target). Preview the whole site exactly as deployed:

```sh
make -C retroweb preview       # foreground, on :8000 — dies with the terminal
make -C retroweb preview-bg    # detached: survives the terminal, gone on reboot
make -C retroweb preview-stop  # stop the detached server
```

Both build the emulator's wasm, fetch the Altair BASIC / CP/M media, stage
`_site/`, and serve `http://0.0.0.0:8000/` (landing page) + `/altair8800/` (the
emulator) on the LAN. A bare `python3 -m http.server` in `retroweb/` will **not**
work — the emulator lives at `altair8800/web/` in source, only at `altair8800/`
in the staged site.

**If the preview keeps dropping** (terminal closed, laptop slept, a crash),
install it as a launchd agent — `RunAtLoad` + `KeepAlive` bring it back:

```sh
make -C retroweb preview-install     # loads ~/Library/LaunchAgents/dev.retroweb.preview.plist
make -C retroweb site                # refresh what it serves, after editing source
make -C retroweb preview-uninstall   # remove it
```

## Adding a machine

New subdir `retroweb/<machine>/` with its own project + `web/` front end; add a
`.machine-card` to `index.html`; add a stage step to the `build` job in
`.github/workflows/deploy-emulator.yml`.
