#!/bin/sh
# Re-run every experiment of the paper.  Takes several hours and ~2 GB of RAM
# per TRIAD job.  Run from the repository root after `make`.
set -e
J=${JOBS:-1}                       # parallel TRIAD jobs (timings are only clean with 1)
sh bench/get_silesia.sh
python3 bench/gen_synthetic.py bench/synthetic
mkdir -p bench/work
S=bench/corpus; Y=bench/synthetic; O=bench/work

# 1. Silesia: references and the three TRIAD levels
python3 bench/run_refs.py --ppmd --out $O/silesia.csv $S/*
for L in 1 2 3; do
  python3 bench/run_triad.py --out $O/silesia.csv --label "TRIAD -$L" --logs $O/logs --jobs $J $S/* -- -$L
done

# 2. synthetic data
python3 bench/run_refs.py --out $O/synthetic.csv $Y/*
for L in 1 2 3; do
  python3 bench/run_triad.py --out $O/synthetic.csv --label "TRIAD -$L" --logs $O/logs --jobs $J $Y/* -- -$L
done

# 3. layer ablation: program search only (-G), expression search only (-P)
python3 bench/run_triad.py --out $O/ablation.csv --label "TRIAD -2 -G (program search only)"    --jobs $J $S/* $Y/* -- -2 -G
python3 bench/run_triad.py --out $O/ablation.csv --label "TRIAD -2 -P (expression search only)" --jobs $J $S/* $Y/* -- -2 -P

# 4. sensitivity to the seed of the search
for s in 1 2 3 4; do
  python3 bench/run_triad.py --out $O/seeds.csv --label "TRIAD -2 -s$s" --jobs $J \
      $S/sao $S/xml $S/ooffice $S/reymont $S/mr $S/osdb -- -2 -s$s
done

# the baselines of 3 and 4 are the runs of 1 and 2: copy those rows under the labels the tables expect
python3 - "$O" <<'PY'
import csv, sys
O = sys.argv[1]
base = [r for n in ('silesia.csv', 'synthetic.csv') for r in csv.DictReader(open(O + '/' + n))]
def add(name, picks, files=None):
    rows = list(csv.DictReader(open(O + '/' + name)))
    have = {r['file'] for r in rows} if files is None else files
    for r in base:
        if r['file'] in have and r['method'] in picks:
            rows.append(dict(r, method=picks[r['method']]))
    with open(O + '/' + name, 'w', newline='') as f:
        w = csv.DictWriter(f, ['file', 'method', 'orig', 'size', 'ctime', 'dtime', 'roundtrip'])
        w.writeheader(); w.writerows(sorted(rows, key=lambda r: r['file']))
add('ablation.csv', {'TRIAD -1': 'TRIAD -1 (no search)', 'TRIAD -2': 'TRIAD -2 (both)'})
add('seeds.csv', {'TRIAD -2': 'TRIAD -2 (default seed)'})
PY
echo "results are in $O/ - tables: python3 bench/make_tables.py $O ; figures: python3 bench/make_figures.py $O figures"
