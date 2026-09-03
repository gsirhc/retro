# retro

Recreations of vintage machines, faithful to the real hardware.

## `retroweb/` — the browser emulators

A static site: [`retroweb/index.html`](retroweb/index.html) is the landing page,
each machine is a subdirectory.

| Machine | Source | Deployed at |
|---|---|---|
| **MITS Altair 8800** (Intel 8080, CP/M 2.2, Altair BASIC) | [`retroweb/altair8800/`](retroweb/altair8800/) | `/altair8800/` |

The Altair project is a C++ 8080 core (`i8080`, 88-2SIO, 88-ACR cassette,
88-DCDD floppy) with a GoogleTest suite, compiled to WebAssembly and wired to an
xterm.js front end in `retroweb/altair8800/web/`. See its
[README](retroweb/altair8800/README.md).

CI (`.github/workflows/deploy-emulator.yml`) runs the C++ and Playwright suites,
stages the site (`retroweb/index.html` at the root, the built front end under
`/altair8800/`), and publishes it to GitHub Pages. It does not deploy a red
build.

## Other directories

`cpu6502`, `commodore64`, `cc65`, `kicad` — earlier / adjacent experiments.
