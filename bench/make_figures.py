#!/usr/bin/env python3
"""Draw the figures of the paper from results/*.csv into figures/.

usage: make_figures.py [RESULTS_DIR] [FIGURES_DIR]
Needs matplotlib.
"""
import csv, os, sys
from collections import defaultdict
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
R = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', 'results')
F = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, '..', 'figures')
os.makedirs(F, exist_ok=True)

SURFACE, INK, INK2, GRID = '#fcfcfb', '#0b0b0b', '#52514e', '#e4e3df'
BLUE, ORANGE, AQUA, NEUTRAL = '#2a78d6', '#eb6834', '#1baf7a', '#a3a29b'
plt.rcParams.update({'font.family': 'DejaVu Sans', 'font.size': 10, 'text.color': INK, 'axes.labelcolor': INK2,
                     'xtick.color': INK2, 'ytick.color': INK2, 'axes.edgecolor': GRID, 'figure.facecolor': SURFACE,
                     'axes.facecolor': SURFACE, 'savefig.facecolor': SURFACE, 'axes.spines.top': False,
                     'axes.spines.right': False, 'axes.titleweight': 'bold', 'axes.titlesize': 11})

def load(name):
    t = defaultdict(dict); order = []
    for r in csv.DictReader(open(os.path.join(R, name))):
        t[r['file']][r['method']] = r
        if r['method'] not in order: order.append(r['method'])
    return t, order

def save(fig, name):
    fig.savefig(os.path.join(F, name), dpi=160, bbox_inches='tight'); plt.close(fig); print('wrote', name)

SIL, ORDER = load('silesia.csv')
FILES = sorted(SIL)
ORIG = sum(int(SIL[f]['TRIAD -1']['orig']) for f in FILES)
TOT = {m: sum(int(SIL[f][m]['size']) for f in FILES) for m in ORDER}
CT = {m: sum(float(SIL[f][m]['ctime']) for f in FILES) for m in ORDER}
DT = {m: sum(float(SIL[f][m]['dtime']) for f in FILES) for m in ORDER}

# Figure 1: total compressed size of the Silesia corpus
ms = sorted(ORDER, key=lambda m: -TOT[m])
fig, ax = plt.subplots(figsize=(7.6, 4.2))
bars = ax.barh(ms, [TOT[m] / 1e6 for m in ms], height=0.62,
               color=[BLUE if m.startswith('TRIAD') else NEUTRAL for m in ms])
for b, m in zip(bars, ms):
    ax.text(b.get_width() + 0.6, b.get_y() + b.get_height() / 2, '%.2f MB  (%.2f : 1)' % (TOT[m] / 1e6, ORIG / TOT[m]),
            va='center', fontsize=9, color=INK)
ax.set_xlim(0, max(TOT.values()) / 1e6 * 1.3)
ax.set_xlabel('compressed size of the 211.9 MB corpus, MB (smaller is better)')
ax.set_title('Silesia corpus: total compressed size', loc='left')
ax.tick_params(length=0); ax.grid(axis='x', color=GRID, linewidth=0.8); ax.set_axisbelow(True)
ax.invert_yaxis()
save(fig, 'silesia_total.png')

# Figure 2: what the search adds, per file
fs = sorted(FILES, key=lambda f: int(SIL[f]['TRIAD -3']['size']) / int(SIL[f]['TRIAD -1']['size']))
g2 = [100 * (int(SIL[f]['TRIAD -2']['size']) / int(SIL[f]['TRIAD -1']['size']) - 1) for f in fs]
g3 = [100 * (int(SIL[f]['TRIAD -3']['size']) / int(SIL[f]['TRIAD -1']['size']) - 1) for f in fs]
fig, ax = plt.subplots(figsize=(7.6, 4.6))
y = range(len(fs)); h = 0.36
ax.barh([i - h / 2 - 0.02 for i in y], g2, height=h, color=BLUE, label='TRIAD -2 (150 search steps)')
ax.barh([i + h / 2 + 0.02 for i in y], g3, height=h, color=ORANGE, label='TRIAD -3 (600 search steps)')
for i in y:
    for v, dy in ((g2[i], -h / 2 - 0.02), (g3[i], h / 2 + 0.02)):
        ax.text(v - 0.4 if v < 0 else v + 0.4, i + dy, '%+.1f%%' % v, va='center', ha='right' if v < 0 else 'left', fontsize=8, color=INK)
ax.set_yticks(list(y)); ax.set_yticklabels(fs); ax.invert_yaxis()
ax.axvline(0, color=INK2, linewidth=0.8)
ax.set_xlim(min(g3 + g2) * 1.18, 4)
ax.set_xlabel('change in compressed size relative to the fixed model, TRIAD -1 (negative is better)')
ax.set_title('Effect of the per-file search on each Silesia file', loc='left')
ax.tick_params(length=0); ax.grid(axis='x', color=GRID, linewidth=0.8); ax.set_axisbelow(True)
ax.legend(frameon=False, loc='lower left', fontsize=9)
save(fig, 'search_gain.png')

# Figure 3: compression ratio against compression time
fig, ax = plt.subplots(figsize=(7.6, 4.4))
for m in ORDER:
    c = BLUE if m.startswith('TRIAD') else NEUTRAL
    ax.scatter(CT[m], ORIG / TOT[m], s=60, color=c, edgecolor=SURFACE, linewidth=2, zorder=3)
    dx, dy, ha = 1.12, 0, 'left'
    if m in ('zstd -19', 'TRIAD -2', 'TRIAD -3'): dx, ha = 0.89, 'right'
    if m == 'xz -9e': dy = 0.06
    ax.text(CT[m] * dx, ORIG / TOT[m] + dy, m, va='center', ha=ha, fontsize=9, color=INK)
ax.set_xscale('log')
ax.set_xlabel('time to compress the whole corpus, seconds (log scale)')
ax.set_ylabel('compression ratio')
ax.set_title('Silesia corpus: ratio against compression time', loc='left')
ax.grid(color=GRID, linewidth=0.8); ax.set_axisbelow(True); ax.tick_params(length=0)
ax.set_xlim(8, 6000)
save(fig, 'ratio_vs_time.png')

# Figure 4: which layer does the work (needs ablation.csv)
if os.path.exists(os.path.join(R, 'ablation.csv')):
    AB, _ = load('ablation.csv')
    AO = ['TRIAD -1 (no search)', 'TRIAD -2 -G (program search only)', 'TRIAD -2 -P (expression search only)', 'TRIAD -2 (both)']
    fs = [f for f in FILES if f in AB]
    fs = sorted(fs, key=lambda f: int(AB[f][AO[3]]['size']) / int(AB[f][AO[0]]['size']))
    series = [(AO[1], 'program search only', AQUA), (AO[2], 'expression search only', ORANGE), (AO[3], 'both (TRIAD -2)', BLUE)]
    fig, ax = plt.subplots(figsize=(7.6, 5.4))
    h = 0.25
    for k, (m, lab, col) in enumerate(series):
        v = [100 * (int(AB[f][m]['size']) / int(AB[f][AO[0]]['size']) - 1) for f in fs]
        ax.barh([i + (k - 1) * (h + 0.02) for i in range(len(fs))], v, height=h, color=col, label=lab)
    ax.set_yticks(range(len(fs))); ax.set_yticklabels(fs); ax.invert_yaxis()
    ax.axvline(0, color=INK2, linewidth=0.8)
    ax.set_xlabel('change in compressed size relative to no search (negative is better)')
    ax.set_title('Layer ablation on the Silesia corpus', loc='left')
    ax.tick_params(length=0); ax.grid(axis='x', color=GRID, linewidth=0.8); ax.set_axisbelow(True)
    ax.legend(frameon=False, loc='lower left', fontsize=9)
    save(fig, 'ablation.png')
