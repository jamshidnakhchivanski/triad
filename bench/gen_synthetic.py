#!/usr/bin/env python3
"""Generate the ten synthetic test files used in the paper.

usage: gen_synthetic.py OUTDIR [--dickens PATH]

All generators use fixed seeds.  Two files depend on things outside this
script and are therefore not bit-reproducible everywhere:
  06_gzipped  needs the Silesia file 'dickens' (give its path with --dickens)
              and depends on the zlib version;
  07_source   concatenates the .py files of the running Python's standard
              library, so it changes with the Python version (the paper used
              CPython 3.13.16).
SHA-256 digests of the files used in the paper are in results/synthetic_sha256.txt.
"""
import random, struct, json, gzip, os, sys, sysconfig, math, argparse

ap = argparse.ArgumentParser()
ap.add_argument('outdir')
ap.add_argument('--dickens', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'corpus', 'dickens'))
a = ap.parse_args()
os.makedirs(a.outdir, exist_ok=True)

def w(name, b):
    open(os.path.join(a.outdir, name), 'wb').write(b)
    print('%-13s %9d' % (name, len(b)))

# 1. quadratic sequence: i*i as 64-bit little-endian integers
w('01_quad', b''.join(struct.pack('<Q', i * i) for i in range(200000)))

# 2. arithmetic sequence + noise: 7*i + rand(0..255) as 32-bit little-endian
r = random.Random(2)
w('02_arith_rnd', b''.join(struct.pack('<I', 7 * i + r.randrange(256)) for i in range(400000)))

# 3. JSON records, one object per line
r = random.Random(3)
first = ['Aysel', 'Murad', 'Leyla', 'Elvin', 'Nigar', 'Rashad', 'Sevda', 'Tural', 'Gunel', 'Kamran', 'Farid', 'Lala']
last = ['Aliyev', 'Mammadova', 'Hasanov', 'Guliyeva', 'Ismayilov', 'Huseynova', 'Abbasov', 'Karimova']
city = ['Baku', 'Ganja', 'Sumqayit', 'Shaki', 'Lankaran', 'Nakhchivan', 'Mingachevir', 'Quba']
tags = ['new', 'vip', 'trial', 'blocked', 'partner', 'student', 'staff']
rows = []; ts = 1700000000
for i in range(14000):
    ts += r.randrange(1, 900)
    f, l = r.choice(first), r.choice(last)
    rows.append(json.dumps({'id': 100000 + i, 'name': f + ' ' + l,
        'email': '%s.%s%d@example.com' % (f.lower(), l.lower(), r.randrange(100)),
        'city': r.choice(city), 'age': r.randrange(18, 80), 'balance': round(r.uniform(0, 25000), 2),
        'active': r.random() < 0.8, 'created': ts, 'tags': r.sample(tags, r.randrange(0, 4)),
        'score': round(r.gauss(50, 15), 3)}))
w('03_json', ('\n'.join(rows) + '\n').encode())

# 4. repeated text: one 313-byte paragraph, 6390 times
para = ("Sixisdirma alqoritmi melumatdaki tekrarlari ve qanunauygunluqlari tapir, sonra onlari daha qisa sekilde yazir. "
        "Itkisiz sixisdirmada ilkin melumat bit-bit geri qaytarilir. Bu abzas sinaq ucun defelerle tekrarlanir: "
        "eyni metn ne qeder cox tekrarlanirsa, yaxsi kompressor onu bir o qeder kicik yere sigisdirmalidir.\n").encode()
w('04_repeat', para * ((2_000_000 // len(para)) + 1))

# 5. pseudo-random bytes
w('05_random', random.Random(5).randbytes(1_000_000))

# 6. already compressed data: first 4 MB of 'dickens', gzip -9
if os.path.exists(a.dickens):
    w('06_gzipped', gzip.compress(open(a.dickens, 'rb').read(4_000_000), 9, mtime=0))
else:
    print('06_gzipped    skipped: %s not found (run bench/get_silesia.sh or pass --dickens)' % a.dickens)

# 7. source code: Python standard library .py files, alphabetical, up to ~3 MB
d = sysconfig.get_paths()['stdlib']; src = b''
for f in sorted(os.listdir(d)):
    if f.endswith('.py'):
        src += open(os.path.join(d, f), 'rb').read()
        if len(src) >= 3_000_000:
            break
w('07_source', src)

# 8. the quadratic sequence as decimal text, one number per line
w('08_quad_text', ''.join('%d\n' % (i * i) for i in range(200000)).encode())

# 9. CSV table: a synthetic sensor log
r = random.Random(9); t = 1700000000; lines = ['timestamp,sensor,temp_c,humidity,status']
for i in range(50000):
    t += r.choice([1, 1, 1, 2, 5]); s = r.randrange(12)
    lines.append('%d,S%02d,%.1f,%d,%s' % (t, s, 20 + 5 * math.sin(i / 300) + r.gauss(0, 0.3) + s * 0.5,
                                           45 + r.randrange(10), r.choice(['OK'] * 18 + ['WARN', 'ERR'])))
w('09_csv', ('\n'.join(lines) + '\n').encode())

# 10. fixed-width binary records, 16 bytes each
r = random.Random(10); recs = []
for i in range(100000):
    recs.append(struct.pack('<IHHfI', 1700000000 + i * 3, i % 500, r.randrange(40), 100.0 + 0.25 * (i % 4000),
                            r.choice([0, 1, 1, 2, 0xFFFF0000])))
w('10_records', b''.join(recs))
