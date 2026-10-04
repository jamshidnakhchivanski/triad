#!/bin/sh
# Round-trip and robustness tests.  Usage: sh tests/run_tests.sh ./triad
# Needs: a C compiler, python3 (to generate inputs), cmp.  Runs in a few minutes.
set -u
T=${1:-./triad}
case "$T" in /*) ;; *) T="$(pwd)/$T" ;; esac
ROOT=$(cd "$(dirname "$0")/.." && pwd)
D="$ROOT/tests/tmp"
rm -rf "$D"; mkdir -p "$D"; cd "$D" || exit 1
fail=0; pass=0
ok()  { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL: $*"; }

python3 - <<'PY'
import random, struct, math
r = random.Random(1)
def w(n, b): open(n, 'wb').write(b)
text = ("The quick brown fox jumps over the lazy dog. Pack my box with five dozen liquor jugs.\n" * 400).encode()
w('empty', b'')
w('one', b'a')
w('tiny', b'hello hello hello\n')
w('text4095', text[:4095]); w('text4096', text[:4096]); w('text4097', text[:4097])
w('text', bytes(r.choice(b"etaoin shrdlu\n") for _ in range(60000)))
w('random', r.randbytes(20000))
w('zeros', bytes(300000))
w('records', b''.join(struct.pack('<IHHf', i * 7, r.randint(0, 50), i % 300, i * 0.25) for i in range(8000)))
w('s16be', b''.join(struct.pack('>h', int(8000 * math.sin(i / 37.0) + r.randint(-30, 30))) for i in range(40000)))
w('u32le_off1', b'\x07' + b''.join(struct.pack('<I', 1000 * i + r.randrange(16)) for i in range(20000)) + b'xy')
w('quad64', b''.join(struct.pack('<Q', i * i) for i in range(12000)))
w('csv', ''.join('%05d,%s,%d.%02d\n' % (i, r.choice(['alpha', 'beta', 'gamma']), r.randint(0, 999), r.randint(0, 99)) for i in range(5000)).encode())
# x86-like: many CALL instructions with small relative targets, some overlapping
b = bytearray()
for i in range(30000):
    k = r.random()
    if k < 0.25:   b += b'\xE8' + struct.pack('<i', r.randrange(-3000, 3000))
    elif k < 0.30: b += b'\xE8\xE8\xE9\x00\xFF'
    else:          b += bytes([r.choice([0x8B, 0x45, 0x89, 0x55, 0xFF, 0x00, 0x83, 0xC4])])
w('x86like', bytes(b))
w('e8storm', bytes(r.choice([0xE8, 0xE9, 0, 0xFF, 1, 0x7F, 0x80]) for _ in range(30000)))
PY

for f in empty one tiny text4095 text4096 text4097 text random zeros records s16be u32le_off1 quad64 csv x86like e8storm; do
    for L in 1 2; do
        if "$T" c -$L -m16 "$f" "$f.$L.trd" && "$T" d "$f.$L.trd" "$f.$L.out" && cmp -s "$f" "$f.$L.out"; then ok
        else bad "round trip $f at level $L"; fi
    done
done

# ablation switches and seeds must also round-trip
for flags in "-2 -P" "-2 -G" "-2 -s7"; do
    if "$T" c $flags -m16 records r.trd && "$T" d r.trd r.out && cmp -s records r.out; then ok; else bad "round trip records with $flags"; fi
done

# the same input and seed must give the same archive
"$T" c -2 -m16 csv a.trd && "$T" c -2 -m16 csv b.trd && cmp -s a.trd b.trd && ok || bad "compression is not deterministic"

# incompressible input is stored: 17 bytes of overhead
sz=$(wc -c < random.2.trd); [ "$sz" -eq 20017 ] && ok || bad "stored size is $sz, expected 20017"

# damaged archives must be rejected, not decoded to wrong data
python3 - <<'PY'
z = bytearray(open('csv.2.trd', 'rb').read())
for name, pos in (('flip_payload', len(z) - 50), ('flip_mid', len(z) // 2)):
    y = bytearray(z); y[pos] ^= 0x55; open(name, 'wb').write(y)
open('truncated', 'wb').write(z[:len(z) // 2])
open('garbage', 'wb').write(bytes(range(40)))
PY
for f in flip_payload flip_mid truncated garbage; do
    if "$T" d "$f" "$f.out" 2>/dev/null; then bad "damaged archive $f was accepted"; else ok; fi
done

# transform steps: fuzz the functions the compressor actually uses
if ${CC:-cc} -O2 -w -o "$ROOT/tests/x86_fuzz" "$ROOT/tests/x86_fuzz.c" -lm && "$ROOT/tests/x86_fuzz"; then ok; else bad "transform fuzz"; fi

echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]
