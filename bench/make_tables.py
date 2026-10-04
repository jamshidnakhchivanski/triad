#!/usr/bin/env python3
"""Print the tables of the paper (Markdown) from results/*.csv.

usage: make_tables.py [RESULTS_DIR]        (default: results/ next to bench/)
"""
import csv, os, sys, statistics
from collections import defaultdict

R = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'results')

CANON = ['gzip -9', 'bzip2 -9', 'zstd -19', 'zstd -22 --long', 'brotli -11', 'xz -6', 'xz -9e', 'PPMd o16',
         'TRIAD -1', 'TRIAD -2', 'TRIAD -3']

def load(name):
    t = defaultdict(dict); order = []
    for r in csv.DictReader(open(os.path.join(R, name))):
        assert r['roundtrip'] == 'ok', r
        t[r['file']][r['method']] = r
        if r['method'] not in order: order.append(r['method'])
    order.sort(key=lambda m: CANON.index(m) if m in CANON else len(CANON))   # stable: unknown labels keep file order
    return t, order

def n(x): return format(int(x), ',')

def size_table(t, methods, total=True, names=None):
    out = ['| File | Original | ' + ' | '.join(methods) + ' |', '|---|' + '---:|' * (len(methods) + 1)]
    tot = defaultdict(int); orig = 0
    for f in sorted(t):
        have = {m: int(t[f][m]['size']) for m in methods if m in t[f]}
        best = min(have.values())
        cells = [('**%s**' % n(have[m]) if have[m] == best else n(have[m])) if m in have else 'n/a' for m in methods]
        o = int(next(iter(t[f].values()))['orig']); orig += o
        out.append('| %s | %s | ' % ((names or {}).get(f, '`%s`' % f), n(o)) + ' | '.join(cells) + ' |')
        for m in have: tot[m] += have[m]
    if total:
        best = min(tot.values())
        out.append('| **Total** | %s | ' % n(orig) + ' | '.join('**%s**' % n(tot[m]) if tot[m] == best else n(tot[m]) for m in methods) + ' |')
        out.append('| Ratio | | ' + ' | '.join('%.3f' % (orig / tot[m]) for m in methods) + ' |')
        out.append('| Bits per byte | | ' + ' | '.join('%.3f' % (8 * tot[m] / orig) for m in methods) + ' |')
    return '\n'.join(out), tot, orig

def silesia():
    t, order = load('silesia.csv')
    tab, tot, orig = size_table(t, order)
    print('### Silesia: compressed size in bytes\n'); print(tab)
    print('\n### Silesia: totals, time\n')
    print('| Compressor | Total (bytes) | Ratio | vs xz -9e | Compress (s) | Decompress (s) | MB/s comp. | MB/s decomp. |\n|---|---:|---:|---:|---:|---:|---:|---:|')
    for m in order:
        ct = sum(float(t[f][m]['ctime']) for f in t); dt = sum(float(t[f][m]['dtime']) for f in t)
        sec = lambda x: '%.1f' % x if x < 10 else '%.0f' % x
        spd = lambda x: '%.2f' % x if x < 10 else '%.0f' % x
        print('| %s | %s | %.3f | %+.1f%% | %s | %s | %s | %s |' % (m, n(tot[m]), orig / tot[m], 100 * (tot[m] / tot['xz -9e'] - 1), sec(ct), sec(dt), spd(orig / 1e6 / ct), spd(orig / 1e6 / max(dt, 0.01))))
    print('\n### Silesia: effect of the search (size change relative to TRIAD -1)\n')
    print('| File | TRIAD -1 | TRIAD -2 | change | TRIAD -3 | change |\n|---|---:|---:|---:|---:|---:|')
    for f in sorted(t):
        a, b, c = (int(t[f]['TRIAD -%d' % k]['size']) for k in (1, 2, 3))
        print('| `%s` | %s | %s | %+.1f%% | %s | %+.1f%% |' % (f, n(a), n(b), 100 * (b / a - 1), n(c), 100 * (c / a - 1)))
    a, b, c = (tot['TRIAD -%d' % k] for k in (1, 2, 3))
    print('| **Total** | %s | %s | %+.1f%% | %s | %+.1f%% |' % (n(a), n(b), 100 * (b / a - 1), n(c), 100 * (c / a - 1)))

def synthetic():
    t, order = load('synthetic.csv')
    tab, _, _ = size_table(t, order, total=False)
    print('\n### Synthetic data: compressed size in bytes\n'); print(tab)

def ablation():
    t, _ = load('ablation.csv')
    order = ['TRIAD -1 (no search)', 'TRIAD -2 -G (program search only)', 'TRIAD -2 -P (expression search only)', 'TRIAD -2 (both)']
    print('\n### Layer ablation: compressed size in bytes and change relative to no search\n')
    print('| File | No search | Program only | change | Expressions only | change | Both | change |\n|---|---:|---:|---:|---:|---:|---:|---:|')
    tot = [0, 0, 0, 0]
    sil = {'dickens', 'mozilla', 'mr', 'nci', 'ooffice', 'osdb', 'reymont', 'samba', 'sao', 'webster', 'x-ray', 'xml'}
    for group in (sorted(f for f in t if f in sil), sorted(f for f in t if f not in sil)):
        for f in group:
            v = [int(t[f][m]['size']) for m in order]
            if f in sil: tot = [x + y for x, y in zip(tot, v)]
            print('| `%s` | %s | %s | %+.1f%% | %s | %+.1f%% | %s | %+.1f%% |' % (f, n(v[0]), n(v[1]), 100 * (v[1] / v[0] - 1), n(v[2]), 100 * (v[2] / v[0] - 1), n(v[3]), 100 * (v[3] / v[0] - 1)))
        if group and group[0] in sil:
            v = tot
            print('| **Silesia total** | %s | %s | %+.1f%% | %s | %+.1f%% | %s | %+.1f%% |' % (n(v[0]), n(v[1]), 100 * (v[1] / v[0] - 1), n(v[2]), 100 * (v[2] / v[0] - 1), n(v[3]), 100 * (v[3] / v[0] - 1)))

def seeds():
    t, order = load('seeds.csv')
    print('\n### Sensitivity to the seed of the search (TRIAD -2, five seeds)\n')
    print('| File | min | max | mean | spread (max−min)/mean | TRIAD -1 | worst seed vs -1 |\n|---|---:|---:|---:|---:|---:|---:|')
    s, _ = load('silesia.csv')
    for f in sorted(t):
        v = [int(r['size']) for r in t[f].values()]; base = int(s[f]['TRIAD -1']['size'])
        print('| `%s` | %s | %s | %s | %.2f%% | %s | %+.1f%% |' % (f, n(min(v)), n(max(v)), n(round(statistics.mean(v))), 100 * (max(v) - min(v)) / statistics.mean(v), n(base), 100 * (max(v) / base - 1)))

if __name__ == '__main__':
    silesia(); synthetic()
    if os.path.exists(os.path.join(R, 'ablation.csv')): ablation()
    if os.path.exists(os.path.join(R, 'seeds.csv')): seeds()
