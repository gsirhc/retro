# retroweb

The browser emulators and the landing page that lists them.

- [`index.html`](index.html) — the landing page (site root). Plain static HTML;
  a `<select id="pageTheme">` that shares the `retro8080.theme` `localStorage`
  key with the emulators, so a theme choice carries across.
- [`altair8800/`](altair8800/) — MITS Altair 8800 (Intel 8080). C++ core +
  GoogleTest + WebAssembly front end in `altair8800/web/`. Deploys to
  `/altair8800/`. See its [README](altair8800/README.md).

## The deployed site

CI builds the front end, fetches the pinned Altair BASIC / CP/M media, then
**stages** the site so URLs are clean:

```
_site/index.html      <- retroweb/index.html
_site/altair8800/     <- retroweb/altair8800/web/  (dev-only files stripped)
```

`_site/` is git-ignored, built by `make -C retroweb site` (CI runs the same
target). Preview the whole site exactly as deployed:

```sh
make -C retroweb preview
# -> http://0.0.0.0:8000/             landing page
# -> http://0.0.0.0:8000/altair8800/  the emulator
```

`make preview` builds the emulator's wasm, fetches the Altair BASIC / CP/M
media, stages `_site/`, and serves it on the LAN. Note that a bare
`python3 -m http.server` in `retroweb/` will **not** work — the emulator lives
at `altair8800/web/` in source, only at `altair8800/` in the staged site.

## Adding a machine

New subdir `retroweb/<machine>/` with its own project + `web/` front end; add a
`.machine-card` to `index.html`; add a stage step to the `build` job in
`.github/workflows/deploy-emulator.yml`.
