# Cassette tapes (`tapes/`)

The 88-ACR deck loads `.cas` files from here and lists them in the
**Load Cassette** dialog. `manifest.json` is the catalog: `{name, file,
description}` per entry.

A `.cas` file is exactly what the emulated ACR's play/record head sees — the raw
byte stream `CSAVE` wrote and `CLOAD` reads back. There's no audio; the deck's
**TAPE SPEED** selector paces the whole transport (Realistic 300 baud / 5x / 25x / 50x).

## `startrek.cas`

Super Star Trek (the 8K-BASIC port in `roms/games/startrek.bas`), tokenised and
`CSAVE`d from Altair 8K BASIC. Regenerate it with the scratch tool that boots
8K BASIC, types the listing in and `CSAVE`s it, then verifies the `CLOAD`
round-trip. It contains no interpreter code — it's the same public-domain
program as the `.bas`, in BASIC's on-tape format.

Tapes you record yourself (`CSAVE` then **SAVE** on the deck) download to your
browser; drop them back in here and add a `manifest.json` line to keep them.
