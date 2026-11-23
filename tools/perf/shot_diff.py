# SPDX-License-Identifier: GPL-3.0-or-later
"""Compares the screenshots of an A/B run (tools/perf/ab_run.ps1) picture by picture.

    python -I tools/perf/shot_diff.py --a shots_on_1 shots_on_2 --b shots_off_1 shots_off_2 [--names on off] [--out diffs] [--markdown]

It reads the `<shot>.png.thumb` files the game writes next to each screenshot with `[gfx] screenshot_thumb = true` (ab_run.ps1 sets
it): the picture averaged down to 320 pixels wide, as raw RGB, so nothing beyond Python's standard library is needed.

For each screenshot name in the first run of both sides:
  - A vs B: how far the two sides' pictures differ (mean difference of the colour channels, 0-255, and the share of the picture that
    differs by more than 24 in some channel);
  - A vs A and B vs B: the same between two runs of one side, when there are two. People, the clock, particles and the upscaler's jitter
    differ from run to run even with the same settings: that is the noise the A vs B figure has to stand out from.
  - "differs" when A vs B is over twice the larger noise and over 1.0, "no noise run" when there is nothing to compare it with.
--out writes <out>/<shot>.png for every shot that differs: A, B and the difference (times 4) side by side, to look at.
"""
import argparse
import os
import struct
import sys
import zlib


def read_thumb(path):
    with open(path, 'rb') as f:
        data = f.read()
    if len(data) < 16 or data[:8] != b'NXTHUMB1':
        return None
    w, h = struct.unpack('<II', data[8:16])
    px = data[16:]
    if len(px) != w * h * 3:
        return None
    return w, h, px


def diff(a, b):
    """(mean channel difference 0-255, % of pixels with a channel off by more than 24) or None when the sizes differ."""
    if not a or not b or a[0] != b[0] or a[1] != b[1]:
        return None
    pa, pb = a[2], b[2]
    total = 0
    over = 0
    n = len(pa) // 3
    for i in range(0, len(pa), 3):
        d0 = abs(pa[i] - pb[i])
        d1 = abs(pa[i + 1] - pb[i + 1])
        d2 = abs(pa[i + 2] - pb[i + 2])
        total += d0 + d1 + d2
        if d0 > 24 or d1 > 24 or d2 > 24:
            over += 1
    return total / (3.0 * n), 100.0 * over / n


def write_png(path, w, h, rgb):
    raw = b''.join(b'\x00' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(kind, body):
        c = struct.pack('>I', len(body)) + kind + body
        return c + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(raw, 6)))
        f.write(chunk(b'IEND', b''))


def side_by_side(a, b):
    w, h = a[0], a[1]
    out = bytearray()
    for y in range(h):
        ra = a[2][y * w * 3:(y + 1) * w * 3]
        rb = b[2][y * w * 3:(y + 1) * w * 3]
        rd = bytes(min(255, 4 * abs(x - z)) for x, z in zip(ra, rb))
        out += ra + rb + rd
    return 3 * w, h, bytes(out)


def shots(d):
    if not d or not os.path.isdir(d):
        return {}
    return {f[:-len('.png.thumb')]: os.path.join(d, f) for f in os.listdir(d) if f.endswith('.png.thumb')}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--a', nargs='+', required=True, help="side A's screenshot folders, one per run")
    ap.add_argument('--b', nargs='+', required=True)
    ap.add_argument('--names', nargs=2, default=['A', 'B'])
    ap.add_argument('--out', default=None)
    ap.add_argument('--markdown', action='store_true')
    a = ap.parse_args()

    sa = [shots(d) for d in a.a]
    sb = [shots(d) for d in a.b]
    first_a = next((s for s in sa if s), {})
    first_b = next((s for s in sb if s), {})
    second_a = next((s for s in sa[1:] if s and s is not first_a), {}) if len(sa) > 1 else {}
    second_b = next((s for s in sb[1:] if s and s is not first_b), {}) if len(sb) > 1 else {}
    names = sorted(set(first_a) & set(first_b))
    if not names:
        print('no screenshots with thumbnails on both sides ([gfx] screenshot_thumb, ab_run.ps1 sets it): no picture comparison')
        return 0
    if a.out:
        os.makedirs(a.out, exist_ok=True)
    na, nb = a.names
    rows = [['screenshot', f'{na} vs {nb}', f'{na} vs {na}', f'{nb} vs {nb}', '']]
    flagged = 0

    def cell(d):
        return '-' if d is None else f'{d[0]:.1f} ({d[1]:.1f}%)'

    for n in names:
        ta, tb = read_thumb(first_a[n]), read_thumb(first_b[n])
        ab = diff(ta, tb)
        aa = diff(ta, read_thumb(second_a[n])) if n in second_a else None
        bb = diff(tb, read_thumb(second_b[n])) if n in second_b else None
        noise = max([x[0] for x in (aa, bb) if x] or [None]) if (aa or bb) else None
        if ab is None:
            verdict = 'sizes differ'
        elif noise is None:
            verdict = 'no noise run'
        elif ab[0] > 2.0 * noise and ab[0] > 1.0:
            verdict = 'DIFFERS'
            flagged += 1
            if a.out:
                w, h, px = side_by_side(ta, tb)
                write_png(os.path.join(a.out, n + '.png'), w, h, px)
        else:
            verdict = 'same within noise'
        rows.append([n, cell(ab), cell(aa), cell(bb), verdict])

    if a.markdown:
        print('| ' + ' | '.join(rows[0]) + ' |')
        print('|' + '---|' * len(rows[0]))
        for r in rows[1:]:
            print('| ' + ' | '.join(r) + ' |')
    else:
        w = [max(len(r[i]) for r in rows) for i in range(len(rows[0]))]
        for r in rows:
            print('  '.join(c.ljust(w[i]) for i, c in enumerate(r)))
    print()
    print(f'Mean colour difference (0-255) and the share of the picture off by more than 24; {flagged} of {len(names)} screenshots differ '
          f'by more than twice the run-to-run noise.' + (f' Side by side (A, B, difference x4) in {a.out}.' if a.out and flagged else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
