#!/usr/bin/env python3
"""Converts one or more zone maps into src/worldmap.h.

    python3 tools/mapconv.py map/world.txt src/worldmap.h

The map is a plain text file, one character per tile. Lines beginning with ';' are
comments and are ignored; every remaining line must be the same length. The width has
may be anything: rows are addressed through an offset table, not by multiplying.

Legend
------
    .   ground              T   tree            /   roof, left end
    :   path                n   rock            ^   roof, middle
    ,   flowers             #   stone wall      \\   roof, right end
    o   stones              ~   water           |   house wall
    "   tall grass          I   sign            O   house wall with window
    u   puddle              C   chest           D   door
    b   dry scrub           Y   cactus          A   dune
    B   wet sand            =   open sea
    @   spawn point         W   well
        (becomes path)

A cottage is drawn straight into the map and looks like one in the text file:

    /^\\        /^^\\
    ODO        |ODO

Ground drawn as '.' is rendered with three interchangeable pictures picked by position,
so a field written with a single character still looks textured. Everything else is
placed exactly as drawn.

Collision follows from the tile value: 0..21 walkable, 22 and above blocking.
"""

import sys

LEGEND = {
    # walkable, 0..16
    '.': 0,  # this zone's ground, auto varied between three pictures
    ':': 3,  # path
    ',': 4,  # flowers
    'o': 5,  # stones
    '"': 6,  # tall grass
    'B': 10, # wet sand
    # blocking, 17 and up
    'T': 17, # tree
    'n': 18, # rock
    '~': 19, # water
    '/': 20, # roof, left end
    '^': 21, # roof, middle
    '\\': 22, # roof, right end
    'O': 23, # house wall with a window
    'D': 24, # door
    'I': 25, # sign
    'C': 26, # chest
    'W': 27, # well
    'Y': 28, # cactus
    'A': 29, # dune
    '=': 30, # open sea
    'P': 31, # pine
    'r': 32, # frozen rock
    'H': 33, # ice wall
    'L': 34, # pillar
    'G': 35, # gate
    '(': 37, # steep roof, left slope
    '_': 38, # its ridge
    ')': 39, # its right slope
    'h': 40, # its wall
    'S': 36, # the shop's door
    '<': 41, # shop board, left half
    '>': 42, # shop board, right half
}
SPAWN = '@'
SPAWN_TILE = 3
SOLID_FROM = 17


def die(msg):
    sys.stderr.write("mapconv: %s\n" % msg)
    sys.exit(1)


def load(path):
    """Reads one zone. Lines starting with ';' are comments, except that a few of
    them carry directives:

        ;zone   meadow          name other zones link to
        ;north  snow            neighbour in that direction, '-' for none
        ;safe   1 1 27 15       rectangle where monsters never spawn, in tiles
        ;mobs   0 1 2           which monster types live here
    """
    rows, spawn = [], None
    meta = {'zone': path, 'links': ['-'] * 4, 'safe': [0, 0, 0, 0], 'mobs': [],
            'ground': 0, 'boss': [0, 0, 0], 'door': [0, 0, 0]}
    keys = {'north': 0, 'south': 1, 'west': 2, 'east': 3}
    with open(path) as f:
        for lineno, line in enumerate(f, 1):
            line = line.rstrip('\n').rstrip('\r')
            if line.startswith(';'):
                parts = line[1:].split()
                if parts:
                    k = parts[0].lower()
                    if k == 'zone' and len(parts) > 1:
                        meta['zone'] = parts[1]
                    elif k in keys and len(parts) > 1:
                        meta['links'][keys[k]] = parts[1]
                    elif k == 'safe' and len(parts) > 4:
                        meta['safe'] = [int(v) for v in parts[1:5]]
                    elif k == 'door' and len(parts) > 3:
                        meta['door'] = parts[1:4]
                    elif k == 'boss' and len(parts) > 3:
                        meta['boss'] = [int(v) for v in parts[1:4]]
                    elif k == 'ground' and len(parts) > 1:
                        meta['ground'] = int(parts[1])
                    elif k == 'mobs':
                        meta['mobs'] = [int(v) for v in parts[1:]]
                continue
            if not line.strip():
                continue
            row = []
            for x, ch in enumerate(line):
                if ch == SPAWN:
                    if spawn is not None:
                        die("line %d: a second spawn point '@'" % lineno)
                    spawn = (x, len(rows))
                    row.append(SPAWN_TILE)
                elif ch in LEGEND:
                    row.append(LEGEND[ch])
                else:
                    die("line %d col %d: character %r is not in the legend"
                        % (lineno, x + 1, ch))
            rows.append((lineno, row))

    if not rows:
        die("the map is empty")
    w = len(rows[0][1])
    for lineno, row in rows:
        if len(row) != w:
            die("line %d is %d tiles wide, expected %d" % (lineno, len(row), w))
    if spawn is None:
        spawn = (w // 2, len(rows) // 2)
    return [r for _, r in rows], w, len(rows), spawn, meta


def packbits(d):
    """PackBits: a token with bit7 set is a run of (token & 0x7f) copies of the next
    byte, otherwise it is a count of literal bytes that follow."""
    out = bytearray()
    i, n = 0, len(d)
    while i < n:
        run = 1
        while i + run < n and d[i + run] == d[i] and run < 127:
            run += 1
        if run >= 3:
            out.append(0x80 | run)
            out.append(d[i])
            i += run
        else:
            lit = bytearray()
            while i < n and len(lit) < 127:
                r2 = 1
                while i + r2 < n and d[i + r2] == d[i] and r2 < 4:
                    r2 += 1
                if r2 >= 3:
                    break
                lit.append(d[i])
                i += 1
            out.append(len(lit))
            out += lit
    return bytes(out)


def unpack(d, w):
    out = bytearray()
    i = 0
    while i < len(d) and len(out) < w:
        t = d[i]; i += 1
        if t & 0x80:
            out += bytes([d[i]]) * (t & 0x7F); i += 1
        else:
            out += d[i:i + t]; i += t
    return bytes(out[:w])


def pack(rows, w, h):
    """Each row is packed on its own so a single row can be decoded without touching
    the rest, which is what the renderer and the collision checks need.

    A row identical to the one above it is stored as nothing at all: a length of zero
    tells the loader to point this row at the same bytes. Maps drawn by hand repeat
    rows often - a corridor, a wall, a shore - and each repeat was costing its full
    compressed size."""
    enc = []
    for k, r in enumerate(rows):
        if k and rows[k - 1] == r:
            enc.append(b"")
        else:
            enc.append(packbits(bytes(r)))
    for k, (r, e) in enumerate(zip(rows, enc)):
        src = e
        j = k
        while src == b"":
            j -= 1
            src = enc[j]
        assert unpack(src, w) == bytes(r), "row does not survive the round trip"
    off, acc = [], 0
    for e in enc:
        off.append(acc)
        acc += len(e)
    off.append(acc)
    return b"".join(enc), off


def reachable(rows, w, h, spawn):
    """Flood fill from the spawn so a hand-edited map cannot quietly grow a pocket
    the player can never get to."""
    seen = [[False] * w for _ in range(h)]
    stack = [spawn]
    seen[spawn[1]][spawn[0]] = True
    n = 0
    while stack:
        x, y = stack.pop()
        n += 1
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = x + dx, y + dy
            if 0 <= nx < w and 0 <= ny < h and not seen[ny][nx] \
                    and rows[ny][nx] < SOLID_FROM:
                seen[ny][nx] = True
                stack.append((nx, ny))
    walk = sum(1 for row in rows for v in row if v < SOLID_FROM)
    cut = []
    for y in range(h):
        for x in range(w):
            if rows[y][x] < SOLID_FROM and not seen[y][x]:
                cut.append((x, y))
    return walk, n, cut


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        die("usage: mapconv.py <zone.txt> [zone2.txt ...] <out.h>")
    srcs, dst = args[:-1], args[-1]

    zones = []
    for path in srcs:
        rows, w, h, spawn, meta = load(path)
        data, off = pack(rows, w, h)
        zones.append(dict(path=path, rows=rows, w=w, h=h, spawn=spawn,
                          meta=meta, data=data, off=off))

    names = {z['meta']['zone']: i for i, z in enumerate(zones)}
    for z in zones:
        for d in range(4):
            n = z['meta']['links'][d]
            if n != '-' and n not in names:
                die("%s links to unknown zone %r" % (z['path'], n))

    maxw = max(z['w'] for z in zones)

    with open(dst, 'w') as f:
        w_ = f.write
        w_("// ==================================================================="
           "================\n")
        w_("// Tiny RPG zones (generated from %s by tools/mapconv.py)\n"
           % ", ".join(z['path'] for z in zones))
        w_("// ==================================================================="
           "================\n")
        w_("// One byte per tile, PackBits compressed a row at a time.\n")
        w_("// Do not edit by hand: change the map files and re-run the converter.\n")
        w_("\n#pragma once\n#include <stdint.h>\n\n")
        w_("#define ZONE_N     %d\n" % len(zones))
        maxh = max(z['h'] for z in zones)
        def mask(v):
            m = 1
            while m < v: m <<= 1
            return m - 1
        w_("#define ZONE_MAXW  %d\n" % maxw)
        w_("#define ZONE_MASKW %d\n" % mask(maxw))
        w_("#define ZONE_MASKH %d\n" % mask(maxh))
        w_("#define ZONE_START 0\n\n")

        # All zones share one blob and one row index. A row is at most 37 bytes
        # compressed, so one byte per row is plenty and halves the index.
        blob, lens, zofs, zrow = bytearray(), bytearray(), [], []
        for z in zones:
            zofs.append(len(blob)); zrow.append(len(lens))
            blob += z['data']
            o = z['off']
            for k in range(len(o) - 1):
                n = o[k + 1] - o[k]
                assert n < 256, "compressed row does not fit in a byte: %d" % n
                lens.append(n)

        w_("// Every zone's rows, one byte each, in zone order\n")
        w_("const uint8_t map_len[%d] = {\n" % len(lens))
        for k in range(0, len(lens), 20):
            w_("  " + ", ".join(str(v) for v in lens[k:k + 20]) + ",\n")
        w_("};\n\n")
        w_("// Every zone's packed rows, back to back\n")
        w_("const uint8_t map_rle[%d] = {\n" % len(blob))
        for k in range(0, len(blob), 16):
            w_("  " + ", ".join("0x%02X" % b for b in blob[k:k + 16]) + ",\n")
        w_("};\n\n")

        w_("typedef struct {\n")
        w_("  uint16_t rleofs;                        // where this zone starts in map_rle\n  uint8_t  rowofs;                        // and where its row lengths start in map_len\n")
        w_("  uint8_t w, h;\n  uint8_t spawnx, spawny;\n")
        w_("  int8_t  link[4];                        // north, south, west, east\n")
        w_("  uint8_t sx0, sy0, sx1, sy1;             // no monsters inside this box\n")
        w_("  uint8_t mob[3], nmob;\n  uint8_t ground;                         // first of this biome's three ground tiles\n  uint8_t boss, bossx, bossy;             // type 0 means the zone has no boss\n  int8_t  door; uint8_t doorx, doory;      // stepping onto this tile leads there\n} ZONE;\n\n")
        w_("const ZONE zones[ZONE_N] = {\n")
        for i, z in enumerate(zones):
            m = z['meta']
            links = [str(names[n]) if n != '-' else "-1" for n in m['links']]
            mobs = (m['mobs'] + [0, 0, 0])[:3]
            w_("  { %d, %d, %d, %d, %d, %d, {%s}, %d,%d,%d,%d, {%d,%d,%d}, %d, %d, %d,%d,%d, %s,%s,%s },"
               "  // %s\n"
               % (zofs[i], zrow[i], z['w'], z['h'], z['spawn'][0], z['spawn'][1], ",".join(links),
                  m['safe'][0], m['safe'][1], m['safe'][2], m['safe'][3],
                  mobs[0], mobs[1], mobs[2], len(m['mobs']), m['ground'],
                  m['boss'][0], m['boss'][1], m['boss'][2],
                  (str(names[m['door'][0]]) if m['door'][0] in names else "-1"),
                  m['door'][1], m['door'][2], m['zone']))
        w_("};\n")

    print("wrote %s" % dst)
    grand = 0
    for i, z in enumerate(zones):
        packed = len(z['data']) + 2 * len(z['off'])
        raw = z['w'] * z['h']
        grand += packed
        walk, got, cut = reachable(z['rows'], z['w'], z['h'], z['spawn'])
        note = "all %d walkable reachable" % walk
        if cut:
            note = "WARNING %d of %d walkable tiles unreachable" % (len(cut), walk)
        print("  %-10s %2dx%-2d %5.1f screens  %4d raw -> %4d (%.2fx)  %s"
              % (z['meta']['zone'], z['w'], z['h'],
                 (z['w'] / 16.0) * (z['h'] / 8.0), raw, packed,
                 float(raw) / packed, note))
    print("  total %d bytes of map data across %d zones" % (grand, len(zones)))


if __name__ == "__main__":
    main()
