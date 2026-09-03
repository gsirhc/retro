# CP/M diagnostic host

`cpm_host` loads a raw `.COM` image at `0x0100` and runs it on the i8080 core,
faking just enough of CP/M to see console output. It's the standard way to run
the classic 8080 test suites.

```sh
make cpm                       # from retroweb/altair8800/  (builds, runs TST8080.COM if present)
./cpm/cpm_host cpm/TST8080.COM  # or run a specific image
./scripts/cpm                   # same, no-arg
```

## How the CP/M calls are mocked

A `.COM` file is a plain memory image of the Transient Program Area. Two low
addresses matter:

| Address | Real CP/M | What `cpm_host` plants |
|---|---|---|
| `0x0000` | warm-boot vector — a finished program jumps/returns here | `OUT 0x00,A` → host sets "finished" |
| `0x0005` | `JMP BDOS` — programs `CALL 0x0005` for OS services | `OUT 0x01,A` then `RET` |

When the CPU executes the planted `OUT` at `0x0005`, the host's `bus.out`
callback runs `bdos_call()`, which reads the request from the CPU registers:

- `C = 0x02` → print the character in `E`
- `C = 0x09` → print the `$`-terminated string pointed to by `DE`

The real `RET` at `0x0007` then returns to the instruction after the program's
`CALL 0x0005`, so the caller's stack is unwound normally — no need to touch
`SP` or `PC` by hand.

## Getting the test binaries

These are public-domain 8080 diagnostics (not redistributed here). Common
sources: the `superzazu/8080` repo's `cpu_tests/` directory, or the
`altairclone.com` / Udo Munk `z80pack` collections.

| File | Notes |
|---|---|
| `8080PRE.COM`  | preliminary smoke test — run this first |
| `TST8080.COM`  | Microcosm Associates 8080/8085 CPU diagnostic, v1.0 |
| `CPUTEST.COM`  | Supersoft diagnostics — longer |
| `8080EXM.COM`  | exhaustive; checks every flag combination (slow) |

Drop them in this directory.

## Expected output — TST8080.COM

```
MICROCOSM ASSOCIATES 8080/8085 CPU DIAGNOSTIC
 VERSION 1.0  (C) 1980

 CPU IS OPERATIONAL
```

`CPU HAS FAILED! ERROR EXIT=xxxx` means the core computed a wrong result or
flag; `xxxx` is the address of the failing check inside the diagnostic.

> Note: `8080EXM.COM` verifies auxiliary-carry and the undocumented flag bits
> far more strictly than `TST8080`. If it reports CRC mismatches, that's a real
> core bug, not a host problem.
