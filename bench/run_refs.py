#!/usr/bin/env python3
"""Run the reference compressors (in memory, through their Python bindings),
verify each round trip, append one CSV row per run.

usage: run_refs.py --out results.csv [--ppmd] FILE...
Needs: pip install zstandard brotli   (and pyppmd for --ppmd)
"""
import argparse, bz2, csv, gzip, lzma, os, time
import brotli, zstandard

ap = argparse.ArgumentParser()
ap.add_argument('--out', required=True); ap.add_argument('--ppmd', action='store_true'); ap.add_argument('files', nargs='+')
a = ap.parse_args()

def zstd_c(d):
    p = zstandard.ZstdCompressionParameters.from_level(22, window_log=27, enable_ldm=True)
    return zstandard.ZstdCompressor(compression_params=p).compress(d)

methods = [
    ('gzip -9',         lambda d: gzip.compress(d, 9),                              gzip.decompress),
    ('bzip2 -9',        lambda d: bz2.compress(d, 9),                               bz2.decompress),
    ('zstd -19',        lambda d: zstandard.ZstdCompressor(level=19).compress(d),   lambda z: zstandard.ZstdDecompressor().decompress(z)),
    ('zstd -22 --long', zstd_c,                                                     lambda z: zstandard.ZstdDecompressor().decompress(z, max_output_size=1 << 31)),
    ('brotli -11',      lambda d: brotli.compress(d, quality=11, lgwin=24),         brotli.decompress),
    ('xz -6',           lambda d: lzma.compress(d, preset=6),                       lzma.decompress),
    ('xz -9e',          lambda d: lzma.compress(d, preset=9 | lzma.PRESET_EXTREME), lzma.decompress),
]
if a.ppmd:
    import pyppmd
    def ppmd_c(d):
        e = pyppmd.Ppmd7Encoder(16, 1 << 30); return e.encode(d) + e.flush()
    methods.append(('PPMd o16', ppmd_c, lambda z, n: pyppmd.Ppmd7Decoder(16, 1 << 30).decode(z, n)))

new = not os.path.exists(a.out)
with open(a.out, 'a', newline='') as f:
    wr = csv.DictWriter(f, ['file', 'method', 'orig', 'size', 'ctime', 'dtime', 'roundtrip'])
    if new: wr.writeheader()
    for path in a.files:
        data = open(path, 'rb').read()
        for name, c, d in methods:
            t = time.time(); z = c(data); ct = time.time() - t
            t = time.time(); back = d(z, len(data)) if name.startswith('PPMd') else d(z); dt = time.time() - t
            row = dict(file=os.path.basename(path), method=name, orig=len(data), size=len(z),
                       ctime=round(ct, 2), dtime=round(dt, 2), roundtrip='ok' if back == data else 'MISMATCH')
            wr.writerow(row); f.flush(); print(row, flush=True)
