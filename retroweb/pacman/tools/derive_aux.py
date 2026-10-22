"""Recover the Ms. Pac-Man aux board's U5/U6/U7 wiring and patch windows from the ROM dumps."""
import argparse
import collections
import itertools
import json
import math
import os
import re
import sys
import zipfile

PROGRAM = ("pacman.6e", "pacman.6f", "pacman.6h", "pacman.6j")
AUX = ("u5", "u6", "u7")
BANKS = (0x3000, 0x3800, 0x8000, 0x8800, 0x9000)
PATCH_TOP = 0x3000
ABS_OPS = {0xC3, 0xCD} | {0xC2 | c << 3 for c in range(8)} | {0xC4 | c << 3 for c in range(8)}


def read_set(path):
    if os.path.isdir(path):
        files = {}
        for n in os.listdir(path):
            with open(os.path.join(path, n), "rb") as f:
                files[n.lower()] = f.read()
        return files
    with zipfile.ZipFile(path) as z:
        return {os.path.basename(n).lower(): z.read(n) for n in z.namelist()}


def load(path):
    roms = read_set(path)
    missing = [n for n in PROGRAM + AUX if n not in roms]
    if missing:
        raise SystemExit("%s lacks %s" % (path, ", ".join(missing)))
    program = b"".join(roms[n][:0x1000] for n in PROGRAM)
    return program, {n: roms[n][:0x800 if n == "u5" else 0x1000] for n in AUX}


def data_table(perm):
    return bytes(sum(((v >> perm[i]) & 1) << i for i in range(8)) for v in range(256))


def addr_table(perm):
    return [sum(((a >> k) & 1) << perm[k] for k in range(len(perm))) for a in range(1 << len(perm))]


def decode(raw, dperm, aperm):
    t = data_table(dperm)
    return bytes(t[raw[x]] for x in addr_table(aperm))


def unigram_logp(program):
    c = collections.Counter(program)
    n = len(program) + 256
    return [math.log((c[v] + 1) / n) for v in range(256)]


def bigram_logp(program, alpha=0.1):
    pairs = collections.Counter(zip(program, program[1:]))
    first = collections.Counter(program[:-1])
    return [[math.log((pairs[(a, b)] + alpha) / (first[a] + 256 * alpha)) for b in range(256)]
            for a in range(256)]


def target_logodds(program):
    # bytes at, and just before, JP/CALL targets in Pac-Man versus anywhere
    at, before, base = collections.Counter(), collections.Counter(), collections.Counter(program)
    n = 0
    for i in range(len(program) - 2):
        if program[i] in ABS_OPS:
            t = program[i + 1] | program[i + 2] << 8
            if 0 < t < len(program):
                at[program[t]] += 1
                before[program[t - 1]] += 1
                n += 1
    N = len(program)

    def lo(c):
        return [math.log((c[v] + 0.5) / (n + 128)) - math.log((base[v] + 0.5) / (N + 128)) for v in range(256)]
    return lo(at), lo(before)


def solve_data_pins(program, aux):
    # a byte histogram ignores address order, so only the data swap moves it
    lp = unigram_logp(program)
    hist = [0] * 256
    for raw in aux.values():
        for b in raw:
            hist[b] += 1
    vals = [v for v in range(256) if hist[v]]
    best = []

    def walk(perm, part):
        i = len(perm)
        if i == 8:
            best.append((sum(hist[v] * lp[part[j]] for j, v in enumerate(vals)), tuple(perm)))
            return
        for p in range(8):
            if p not in perm:
                walk(perm + [p], [d | ((v >> p) & 1) << i for d, v in zip(part, vals)])
    walk([], [0] * len(vals))
    best.sort(reverse=True)
    return best


def greedy_low_lines(raw, lines, depth, bigram):
    # pick the chip line for A0, A1, ... by the pairs (a, a+1) that carry into line k
    used, margins = [], []
    for k in range(depth):
        scores = []
        for p in range(lines):
            if p in used:
                continue
            rest = [q for q in range(lines) if q not in used and q != p]
            ones = sum(1 << q for q in used)
            s = 0.0
            for hi in range(1 << len(rest)):
                x = ones | sum(1 << q for i, q in enumerate(rest) if hi >> i & 1)
                s += bigram[raw[x]][raw[(x & ~ones) | 1 << p]]
            scores.append((s / (1 << len(rest)), p))
        scores.sort(reverse=True)
        used.append(scores[0][1])
        margins.append(scores[0][0] - scores[1][0] if len(scores) > 1 else None)
    return used, margins


def abs_targets(images):
    out = []
    for img in images:
        for i in range(len(img) - 2):
            if img[i] in ABS_OPS:
                out.append(img[i + 1] | img[i + 2] << 8)
    return out


def bank_score(piece, bank, targets, la, lb):
    s = 0.0
    for t in targets:
        o = t - bank
        if 0 < o < len(piece):
            s += la[piece[o]] + lb[piece[o - 1]]
    return s


def place_u6_u7(raw, dperm, low, targets, la, lb, banks_free):
    # remaining four lines and the 2K banks of $3000-$3FFF/$8000-$97FF, scored by JP/CALL targets
    tables = {}
    for chip in ("u6", "u7"):
        rest = [q for q in range(12) if q not in low[chip]]
        for hi in itertools.permutations(rest):
            img = decode(raw[chip], dperm, low[chip] + list(hi))
            tables[(chip, hi)] = [[bank_score(img[h * 0x800:(h + 1) * 0x800], b, targets, la, lb)
                                   for b in banks_free] for h in range(2)]
    ranked = []
    for (_, h6), s6 in ((k, v) for k, v in tables.items() if k[0] == "u6"):
        for (_, h7), s7 in ((k, v) for k, v in tables.items() if k[0] == "u7"):
            for banks in itertools.permutations(range(len(banks_free)), 4):
                s = s6[0][banks[0]] + s6[1][banks[1]] + s7[0][banks[2]] + s7[1][banks[3]]
                ranked.append((s, h6, h7, banks))
    ranked.sort(reverse=True)
    return ranked


def u5_pointers(data, low, bank):
    # chunks of four words all inside U5's own bank are a pointer table
    lowmask = sum(1 << q for q in low)
    chunks = collections.defaultdict(lambda: bytearray(8))
    for x in range(0x800):
        k = sum(((x >> q) & 1) << i for i, q in enumerate(low))
        chunks[x & ~lowmask][k] = data[x]
    ptrs = set()
    for ch in chunks.values():
        words = [ch[i] | ch[i + 1] << 8 for i in range(0, 8, 2)]
        if all(bank < w < bank + 0x800 for w in words):
            ptrs.update(words)
    return sorted(ptrs)


def u5_high_lines(data, low, bank, ptrs):
    # the table's targets share a lead-in byte and a first byte only in the right line order
    def entropy(c):
        n = sum(c.values())
        return -sum(v / n * math.log(v / n) for v in c.values())
    rest = [q for q in range(11) if q not in low]
    ranked = []
    for hi in itertools.permutations(rest):
        amap = low + list(hi)
        at, before = collections.Counter(), collections.Counter()
        for p in ptrs:
            o = p - bank
            at[data[sum(((o >> k) & 1) << amap[k] for k in range(11))]] += 1
            before[data[sum((((o - 1) >> k) & 1) << amap[k] for k in range(11))]] += 1
        ranked.append((entropy(at) + entropy(before), hi))
    ranked.sort()
    return ranked


MEM_HL = {0x34, 0x35, 0x46, 0x4E, 0x56, 0x5E, 0x66, 0x6E, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x77, 0x7E,
          0x86, 0x8E, 0x96, 0x9E, 0xA6, 0xAE, 0xB6, 0xBE}


def base_len(op):
    x, y, z = op >> 6, (op >> 3) & 7, op & 7
    if x == 0:
        return {0: 1 if y < 2 else 2, 1: 3 if y % 2 == 0 else 1, 2: 3 if y >= 4 else 1, 6: 2}.get(z, 1)
    if x in (1, 2):
        return 1
    if z in (2, 4) or (z == 3 and y == 0) or (z == 5 and y == 1):
        return 3
    if (z == 3 and y in (2, 3)) or z == 6:
        return 2
    return 1


def ins_len(m, i):
    op, nxt = m[i], m[i + 1] if i + 1 < len(m) else 0
    if op == 0xCB:
        return 2
    if op == 0xED:
        return 4 if 0x40 <= nxt < 0x80 and nxt & 7 == 3 else 2
    if op in (0xDD, 0xFD):
        if nxt in (0xCB, 0x36):
            return 4
        if nxt in (0xDD, 0xFD, 0xED):
            return 1
        return 1 + base_len(nxt) + (nxt in MEM_HL)
    return base_len(op)


def rejoins(program, w, chunk):
    # the patched bytes fall back onto Pac-Man's own instruction starts, or leave by JP/JR/RET
    starts, i = set(), max(0, w - 64)
    while i < w + 24:
        starts.add(i)
        i += ins_len(program, i)
    patched = bytearray(program)
    patched[w:w + 8] = chunk
    i = min(x for x in starts if x >= w - 3 and x + ins_len(program, x) > w)
    while i < w + 8:
        op = patched[i]
        if op in (0xC3, 0x18, 0xC9, 0xE9):
            return True
        i += ins_len(patched, i)
    return i in starts


def diff_runs(a, b):
    runs, prev = 0, False
    for x, y in zip(a, b):
        cur = x != y
        runs += cur and not prev
        prev = cur
    return runs


def match_windows(u5img, program, table_start):
    # U5 pins 3 and 22 are Z80 A6/A7 (aux schematic M051-00617-B002), so a window keeps those bits
    out, notes = [], []
    for c in range(0, table_start, 8):
        chunk = u5img[c:c + 8]
        if chunk == b"\xff" * 8:
            continue
        scored = [(sum(x == y for x, y in zip(chunk, program[w:w + 8])), w) for w in range(0, PATCH_TOP, 8)]
        top = max(s for s, _ in scored)
        best = [w for s, w in scored if s == top]
        wired = [w for w in best if (w ^ c) & 0xC0 == 0]
        if len(best) == 1 and not wired:
            notes.append("$%04X: best window $%04X breaks the A6/A7 wiring" % (0x8000 + c, best[0]))
        pick = wired or best
        if len(pick) > 1:
            fewest = min(diff_runs(chunk, program[w:w + 8]) for w in pick)
            pick = [w for w in pick if diff_runs(chunk, program[w:w + 8]) == fewest]
        runner = max(s for s, w in scored if w not in pick and (w ^ c) & 0xC0 == 0)
        if len(best) > 1:
            notes.append("$%04X: %d-way tie at %d/8 (%s) -> $%04X" % (
                0x8000 + c, len(best), top, " ".join("$%04X" % w for w in best), pick[0]))
        if len(pick) != 1:
            raise SystemExit("window for U5 $%04X is ambiguous: %s" % (0x8000 + c, pick))
        if top - runner < 2:
            notes.append("$%04X: %d/8 at $%04X, next wired window %d/8" % (0x8000 + c, top, pick[0], runner))
        out.append({"window": pick[0], "u5": c, "match": top, "runner_up": runner,
                    "rejoins": rejoins(program, pick[0], chunk)})
    return out, notes


def derive(path, log=print):
    program, raw = load(path)
    bigram = bigram_logp(program)
    la, lb = target_logodds(program)

    dranked = solve_data_pins(program, raw)
    dperm = dranked[0][1]
    log("data pins: D0-D7 <- chip D%s (margin %.0f nats over %s)" % (
        ",".join(map(str, dperm)), dranked[0][0] - dranked[1][0], dranked[1][1]))

    data5 = bytes(data_table(dperm)[b] for b in raw["u5"])
    low5, margins5 = greedy_low_lines(data5, 11, 6, bigram)
    log("u5 A0-A5 <- chip A%s (bigram margins %s)" % (
        ",".join(map(str, low5)), " ".join("%.2f" % m for m in margins5)))
    found = sorted(((len(u5_pointers(data5, low5[:3], b)), b) for b in BANKS), reverse=True)
    log("u5 pointer-table words per bank: %s" % " ".join("$%04X:%d" % (b, n) for n, b in found))
    u5bank = found[0][1]
    ptrs = u5_pointers(data5, low5[:3], u5bank)
    pranked = u5_high_lines(data5, low5[:3], u5bank, ptrs)
    u5map = low5[:3] + list(pranked[0][1])
    log("u5 at $%04X, A3-A10 <- chip A%s from %d table pointers (lead-in entropy %.2f, next %.2f)" % (
        u5bank, ",".join(map(str, pranked[0][1])), len(ptrs), pranked[0][0], pranked[1][0]))
    if u5map[:6] != low5:
        log("  bigram and pointer table disagree on U5 A3-A5")

    low = {}
    for chip in ("u6", "u7"):
        data = bytes(data_table(dperm)[b] for b in raw[chip])
        low[chip], margins = greedy_low_lines(data, 12, 8, bigram)
        log("%s A0-A7 <- chip A%s (bigram margins %s)" % (
            chip, ",".join(map(str, low[chip])), " ".join("%.2f" % m for m in margins)))
    if low["u6"] != low["u7"]:
        raise SystemExit("U6 and U7 disagree on A0-A7")

    free = [b for b in BANKS if b != u5bank]
    pre = {c: decode(raw[c], dperm, low[c] + [q for q in range(12) if q not in low[c]]) for c in ("u6", "u7")}
    targets = abs_targets([program, pre["u6"], pre["u7"]])
    ranked = place_u6_u7(raw, dperm, low, targets, la, lb, free)
    s, h6, h7, banks = ranked[0]
    alt = next(r for r in ranked if (r[1], r[2]) != (h6, h7))
    alt_b = next(r for r in ranked if r[3] != banks)
    log("u6/u7 A8-A11 <- chip A%s / A%s (margin %.1f over the next line order)" % (
        ",".join(map(str, h6)), ",".join(map(str, h7)), s - alt[0]))
    log("u6 halves at %s, u7 halves at %s (margin %.1f over the next layout)" % (
        " ".join("$%04X" % free[b] for b in banks[:2]), " ".join("$%04X" % free[b] for b in banks[2:]), s - alt_b[0]))
    u6map, u7map = low["u6"] + list(h6), low["u7"] + list(h7)

    u5img = decode(raw["u5"], dperm, u5map)
    table = min(c for c in range(0, 0x800, 8)
                if all(u5bank < (u5img[c + i] | u5img[c + i + 1] << 8) < u5bank + 0x800 for i in range(0, 8, 2)))
    windows, notes = match_windows(u5img, program, table)
    for n in notes:
        log("  " + n)
    loose = [w for w in windows if not w["rejoins"]]
    log("%d of %d windows rejoin Pac-Man's instruction stream%s" % (
        len(windows) - len(loose), len(windows),
        "" if not loose else "; not: " + " ".join("$%04X" % w["window"] for w in loose)))
    log("%d patch windows from U5 $%04X-$%04X" % (len(windows), u5bank, u5bank + table - 1))

    return {
        "data_pin": list(dperm),
        "u5_addr_pin": u5map,
        "u6u7_addr_pin": u6map if u6map == u7map else None,
        "u6_addr_pin": u6map,
        "u7_addr_pin": u7map,
        "u5_base": u5bank,
        "u6_halves": [free[banks[0]], free[banks[1]]],
        "u7_halves": [free[banks[2]], free[banks[3]]],
        "patch_source": [u5bank, u5bank + table - 1],
        "windows": [[w["window"], u5bank + w["u5"]] for w in windows],
    }


def core_tables(path):
    src = open(path).read()

    def ints(name):
        m = re.search(r"\b%s\[\d*\]\s*=\s*\{(.*?)\};" % name, src, re.S)
        if not m:
            raise SystemExit("%s: no %s table" % (path, name))
        return [int(v, 0) for v in re.findall(r"0x[0-9A-Fa-f]+|\d+", m.group(1))]
    patches = ints("kAuxPatches")
    return {
        "data_pin": ints("kAuxDataPin"),
        "u5_addr_pin": ints("kU5AddrPin"),
        "u6u7_addr_pin": ints("kU6U7AddrPin"),
        "windows": sorted([patches[i + 1], 0x8000 + patches[i]] for i in range(0, len(patches), 2)),
    }


def check(out, core_path):
    core = core_tables(core_path)
    # bank layout as Machine::rebuild_aux decodes it
    core.update(u5_base=0x8000, u6_halves=[0x9000, 0x8800], u7_halves=[0x3000, 0x3800])
    bad = 0
    for key, want in core.items():
        got = sorted(out[key]) if key == "windows" else out[key]
        if got == want:
            continue
        bad += 1
        if key == "windows":
            only = [" ".join("$%04X<-$%04X" % tuple(w) for w in a if w not in b) for a, b in ((got, want), (want, got))]
            print("MISMATCH windows: dumps only %s; core only %s" % tuple(only))
        else:
            print("MISMATCH %s: dumps give %s, core has %s" % (key, got, want))
    print("derive-aux-check: %s" % ("%d table(s) differ" % bad if bad else "every table matches machine.cpp"))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("zip", nargs="?", default=os.path.expanduser("~/images/arcade/mspacman.zip"))
    ap.add_argument("--json", action="store_true", help="print the recovered tables as JSON")
    ap.add_argument("--check", metavar="MACHINE_CPP", help="compare with the tables in machine.cpp")
    args = ap.parse_args()
    if not os.path.exists(args.zip):
        print("derive_aux: %s not found, skipping" % args.zip, file=sys.stderr)
        return 0 if args.check else 2
    log = (lambda s: print(s, file=sys.stderr)) if args.json else print
    out = derive(args.zip, log)
    if args.check:
        return check(out, args.check)
    if args.json:
        print(json.dumps(out))
    else:
        for w, src in out["windows"]:
            print("  $%04X-$%04X <- $%04X" % (w, w + 7, src))
    return 0


if __name__ == "__main__":
    sys.exit(main())
