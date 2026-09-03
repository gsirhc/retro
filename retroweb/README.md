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

`_site/` is git-ignored. Preview it exactly as deployed:

```sh
make -C retroweb/altair8800/web retro8080.js roms media
rm -rf retroweb/_site && mkdir -p retroweb/_site/altair8800
cp retroweb/index.html retroweb/_site/
cp -r retroweb/altair8800/web/. retroweb/_site/altair8800/
python3 -m http.server -d retroweb/_site 8000
# -> http://localhost:8000/            landing page
# -> http://localhost:8000/altair8800/ the emulator
```

## Adding a machine

New subdir `retroweb/<machine>/` with its own project + `web/` front end; add a
`.machine-card` to `index.html`; add a stage step to the `build` job in
`.github/workflows/deploy-emulator.yml`.
