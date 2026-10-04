#!/usr/bin/env python3
"""Run TRIAD on files, verify the round trip, append one CSV row per run.

usage: run_triad.py --out results.csv --label "TRIAD -2" [--triad ./triad]
                    [--logs DIR] [--jobs N] FILE... -- FLAGS...

Example: python3 bench/run_triad.py --out r.csv --label "TRIAD -2" bench/corpus/* -- -2
Times are wall-clock seconds; with --jobs > 1 they include contention.
"""
import argparse, csv, filecmp, os, subprocess, sys, tempfile, time
from concurrent.futures import ThreadPoolExecutor

argv = sys.argv[1:]
flags = []
if '--' in argv:
    k = argv.index('--'); flags = argv[k + 1:]; argv = argv[:k]
ap = argparse.ArgumentParser()
ap.add_argument('--out', required=True); ap.add_argument('--label', required=True)
ap.add_argument('--triad', default='./triad'); ap.add_argument('--logs'); ap.add_argument('--jobs', type=int, default=1)
ap.add_argument('files', nargs='+')
a = ap.parse_args(argv)
if a.logs: os.makedirs(a.logs, exist_ok=True)

def run(path):
    name = os.path.basename(path)
    with tempfile.TemporaryDirectory() as tmp:
        z, back = os.path.join(tmp, 'z.trd'), os.path.join(tmp, 'back')
        t0 = time.time()
        c = subprocess.run([a.triad, 'c', '-v', *flags, path, z], capture_output=True, text=True)
        t1 = time.time()
        if c.returncode: return dict(file=name, method=a.label, orig=os.path.getsize(path), size='', ctime='', dtime='', roundtrip='COMPRESS FAILED')
        d = subprocess.run([a.triad, 'd', z, back], capture_output=True, text=True)
        t2 = time.time()
        ok = d.returncode == 0 and filecmp.cmp(path, back, shallow=False)
        if a.logs: open(os.path.join(a.logs, '%s.%s.log' % (name, a.label.replace(' ', '_'))), 'w').write(c.stderr)
        return dict(file=name, method=a.label, orig=os.path.getsize(path), size=os.path.getsize(z),
                    ctime=round(t1 - t0, 2), dtime=round(t2 - t1, 2), roundtrip='ok' if ok else 'MISMATCH')

new = not os.path.exists(a.out)
with open(a.out, 'a', newline='') as f, ThreadPoolExecutor(a.jobs) as ex:
    wr = csv.DictWriter(f, ['file', 'method', 'orig', 'size', 'ctime', 'dtime', 'roundtrip'])
    if new: wr.writeheader()
    for row in ex.map(run, a.files):
        wr.writerow(row); f.flush(); print(row, flush=True)
