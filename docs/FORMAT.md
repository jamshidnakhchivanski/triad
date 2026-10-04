# The TRIAD archive format (version `TRD2`)

This document is the reference for the byte layout written by `src/triad.c`.
All multi-byte integers are little-endian. One archive holds one file.

## 1. Header

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 4 | magic | the ASCII bytes `TRD2` |
| 4 | 1 | mode | `0` stored, `1` coded with a model description, `2` coded with the default model |
| 5 | 8 | n | length of the original file in bytes (must be below 2^32 − 16) |
| 13 | 4 | crc | CRC-32 (polynomial `0xEDB88320`, as in zlib) of the original file |

What follows depends on `mode`.

- **mode 0 (stored).** Exactly `n` bytes: the original file. Used when coding
  would not make the file smaller. Total overhead: 17 bytes.
- **mode 2 (default model).** One byte `tb`, then the coded payload. The decoder
  uses the built-in default model (section 4) and an empty program.
- **mode 1 (described model).** One byte `tb`, then the model description
  (section 2), then the coded payload.

`tb` (16…24) is the base-2 logarithm of the number of buckets in each context's
hash table. The decoder must allocate the same size, because hash collisions
are part of the model.

## 2. Model description (mode 1 only)

```
program      := count:u8 { op }*count                count <= 4
op           := kind:u8 width:u8 big:u8 offset:u8 stride:u16
genome       := slots:u8 { expr }*slots  expr(selector)  expr(correction)  minlen:u8
expr         := atoms:u8 lim:u8 { atom }*atoms       atoms <= 4, lim <= 15
atom         := type:u8 mask:u8 a:u16 c:u16
```

`slots` is between 1 and 14.

### 2.1 Program steps

| kind | Name | Fields used | Forward transform (encoder) |
|---:|---|---|---|
| 1 | `X86` | none | for every position `i`, visited from the end of the file to the start: if byte `i` is `E8` or `E9` and byte `i+4` is `00` or `FF`, add `i` to the 32-bit little-endian operand at `i+1…i+4`, keep the low 25 bits and sign-extend them |
| 2 | `DELTA` | `width` ∈ {1,2,4}, `big` ∈ {0,1}, `offset`, `stride` ≥ 1 | view the bytes from `offset` on as `width`-byte integers (`big` = 1: big-endian); replace element `i` by element `i` − element `i − stride`, modulo 2^(8·width) |

The decoder applies the inverse steps in reverse order. The inverse of `X86`
subtracts `i` and visits positions from the start of the file to the end; the
inverse of `DELTA` adds, visiting elements in increasing order. Trailing bytes
that do not fill a whole element are left unchanged.

### 2.2 Atoms

Let `pos` be the number of bytes already known and `x[pos − d]` the byte at
distance `d` back (distance 1 is the previous byte).

| type | Name | Parameters | Value |
|---:|---|---|---|
| 1 | `ORDk` | `a` = k, 1…24 | hash of the last `k` bytes |
| 2 | `Bd&m` | `a` = d ≥ 1, `mask` | `x[pos − d] AND M[mask]`; the value 256 if `d > pos` |
| 3 | `COLp` / `LINECOL` | `a` = p | `pos mod p` if `p > 0`; otherwise the number of bytes since the last newline; capped at 65535 |
| 4 | `WORD` | none | hash of the letters of the current word so far (ASCII letters are case-folded; bytes ≥ 128 count as letters); 0 between words |
| 5 | `WORD2` | none | hash of the previous complete word |
| 6 | `UPk` | `a` = k | the byte `k` columns to the right of the current column in the previous line; 256 if that line is shorter |
| 7 | `GRAD(a,c)&m` | `a`, `c` ≥ 1, `mask` | `(x[pos−a] + x[pos−c] − x[pos−a−c]) AND M[mask]` |
| 8 | `AVG(a,c)&m` | `a`, `c` ≥ 1, `mask` | `((x[pos−a] + x[pos−c]) >> 1) AND M[mask]` |

Mask table `M`: `FF F0 E0 C0 80 DF 0F FC` (index 0…7). Bytes before the start
of the file read as 0 in `GRAD` and `AVG`. Fields an atom does not use are
ignored by the decoder.

### 2.3 Expressions

The value of an expression is a fold over its atoms:

```
x = salt
for each atom k:  x = hmix(x + type_k * 0x632BE5AB, value_k)
hmix(h, v) = ((h XOR v) * 0x9E3779B1) XOR (that >> 15)        (32-bit arithmetic)
```

An expression with zero atoms is the order-0 context. `lim` (1…15; 0 means 15)
is the count at which the slot's bit counters stop slowing down: a small `lim`
makes the slot adapt faster. For the selector and correction expressions `lim`
is unused.

`minlen` (clamped to 2…32) is the number of bytes hashed by the match model.

## 3. Coded payload

The payload is the output of a 32-bit binary arithmetic coder driven by the
predictor, most significant bit of each byte first, `8·n` bits in total. The
coder state is `x1 = 0`, `x2 = 0xFFFFFFFF`; for a 12-bit probability `p` that
the next bit is 1:

```
xmid = x1 + ((x2 − x1) * p >> 12)          (the product is 64-bit)
bit 1: x2 = xmid        bit 0: x1 = xmid + 1
while the top bytes of x1 and x2 are equal: output that byte, shift both left by 8 (x2 gets 0xFF)
```

At the end the encoder writes the four bytes of `x1`. The decoder reads past
the end of the archive as zeros.

The predictor is specified by the functions `predict` and `update` in
`src/triad.c`; it uses integer arithmetic only, so the same archive decodes on
any platform on which `>>` of a negative `int` is an arithmetic shift.

## 4. The default model

Used in mode 2 and as the starting point of the search:

```
contexts : ORD1 ORD2 ORD3 ORD4 ORD5 ORD6 ORD8 WORD WORD+WORD2 B2+B3 B1+B3 ORD0 B1+WORD2 ORD12
selector : B1        correction : ORD2        minlen : 6        program : empty
```

## 5. Integrity

After decoding and after undoing the program the decoder recomputes the CRC-32
and refuses to write the output if it differs. Header fields are range-checked
(atom types, mask indices, widths, table size, and a plausibility bound of
4800:1 on the compression ratio) before any memory is allocated.
