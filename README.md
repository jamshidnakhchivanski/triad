# TRIAD

**A lossless compressor that searches for the model of each file and stores
what it found in the archive.**

TRIAD is a research prototype (one C file, about 1000 lines). It combines
three layers, one per programming paradigm:

| Layer | What it is | Stored in the archive |
|---|---|---|
| **Procedural** | a short program of reversible steps (x86 address fix-up, typed deltas) | yes, a few bytes |
| **Functional** | the contexts of a context-mixing predictor, written as expressions over eight combinators | yes, 6 bytes per atom |
| **Iterative** | a search, in the encoder only, that changes the program and the expressions and keeps what makes the coded sample smaller | no |

The decoder is a generic interpreter: it knows nothing about file formats.

> **Main idea.** Do not build the model into the compressor. Let the encoder
> search for the model of each file, in a small language of composable context
> functions and reversible steps, with the real coded size as the only judge,
> and write the winning model into the archive so that one generic decoder can
> run it.

The full write-up, with method, experiments, ablation and limitations, is in
**[PAPER.md](PAPER.md)**. The archive format is in
[docs/FORMAT.md](docs/FORMAT.md).

## Results in brief

Silesia corpus, 12 files, 211.9 MB. Every archive was decompressed and
compared with the original.

| Compressor | Size (MB) | Ratio | vs `xz -9e` | Compress (s) | Decompress (s) |
|---|---:|---:|---:|---:|---:|
| gzip -9 | 67.64 | 3.133 | +39.6 % | 19 | 0.9 |
| bzip2 -9 | 54.51 | 3.888 | +12.5 % | 17 | 7.2 |
| zstd -19 | 52.89 | 4.007 | +9.2 % | 94 | 0.3 |
| zstd -22 --long | 52.36 | 4.048 | +8.1 % | 139 | 0.3 |
| brotli -11 | 49.56 | 4.276 | +2.3 % | 473 | 0.8 |
| xz -6 | 49.23 | 4.305 | +1.6 % | 104 | 3.4 |
| xz -9e | 48.46 | 4.374 | +0.0 % | 140 | 3.2 |
| PPMd o16 | 47.30 | 4.480 | -2.4 % | 53 | 55 |
| **TRIAD -1** | 41.34 | 5.127 | -14.7 % | 409 | 421 |
| **TRIAD -2** | 38.59 | 5.492 | -20.4 % | 1080 | 381 |
| **TRIAD -3** | 38.02 | 5.574 | -21.5 % | 3004 | 411 |

![Total compressed size of the Silesia corpus](figures/silesia_total.png)

- Against the same coder with a fixed model (`-1`), the per-file search
  removes 6.6 % of the output at `-2` and 8.0 % at `-3`; on the
  chemical database `nci` it removes 27.3 %.
- On a synthetic file of 64-bit squares (1.6 MB) the search finds a two-step
  delta program and writes **783 bytes**; `xz -9e` writes 543,328.
- TRIAD **loses** on long exact repeats (1,259 bytes against 209 for Brotli on
  a paragraph repeated 6,390 times) and gains nothing on random or
  already-compressed data.

An example of what the search finds, with nothing in the encoder knowing the
format: on the star catalogue `sao` it settles on the contexts `COL28` and
`B28` (the file has 28-byte records); on the image `x-ray` it chooses a 16-bit
little-endian delta and contexts at the row distance.

## What it costs

TRIAD is **slow and memory-hungry**: about 0.5 MB/s for compression *and*
decompression, plus 45–90 s of search per file at `-2`, and up to 1.9 GB of
RAM at the default table size. `xz` decompresses the same corpus more than a
hundred times faster. This is a study of an idea, not a replacement for
`zstd` or `xz`, and it is not a compression record either: paq8px and cmix
compress further (they were not measured here).

## Build and use

```sh
make                              # needs a C99 compiler; builds ./triad
make test                         # 42 round-trip and robustness checks

./triad c -2 -v input out.trd     # compress (-v shows the search and the model found)
./triad d out.trd restored        # decompress
```

| Option | Meaning |
|---|---|
| `-1` | fixed default model, no search |
| `-2` | search, 150 steps (default) |
| `-3` | search, 600 steps |
| `-v` | print the search trace and the final model |
| `-mN` | 2^N hash buckets per context, 16…24 (default 22; `-m20` uses about 0.45 GB) |
| `-sN` | seed of the search |
| `-P`, `-G` | ablation: no program search, no expression search |

What `-v` prints for the star catalogue:

```
sample: 262144 bytes; repeat distances: 28 8112 8056 1667 771 211
  no program: 170402 bytes on the sample
  step   0: 170402 -> 168970  ORD0 => ORD1+B8056
  ...
program : (none)
contexts: ORD1 ORD2 ORD3 ORD2+B6&DF B211/3 COL28 ORD1+B3 B7&DF+B8112+B28 B7+B8
mixer select: B6   correction: B771&C0   match length: 6
7251944 -> 3943522 bytes (4.350 bits/byte)
```

## Repository layout

```
src/triad.c            the compressor: transform program, expression interpreter,
                       context-mixing predictor, search, container
docs/FORMAT.md         byte-level specification of the archive
PAPER.md               the paper
tests/                 round-trip tests, damaged-archive tests, transform fuzzer
bench/                 scripts that reproduce every experiment, table and figure
results/               raw measurements (CSV) and the search trace of every run
figures/               figures of the paper
```

## Reproducing the experiments

```sh
pip install zstandard brotli pyppmd matplotlib
make
JOBS=2 sh bench/reproduce.sh      # downloads Silesia, runs everything; several hours
python3 bench/make_tables.py      # tables from results/*.csv
python3 bench/make_figures.py     # figures
```

Compressed sizes are deterministic. Times depend on the machine; the ones in
the paper were taken on a shared 2-vCPU virtual machine and are indicative.

## Status and limitations

- Research prototype. The format (`TRD2`) may change.
- One file per archive, whole file in memory, at most 4 GB.
- The search looks at a 256 KiB sample, so it can pick a model that does not
  suit the rest of the file; on one Silesia file (`samba`) level `-2` is 0.6 %
  worse than `-1`.
- The outcome depends on the seed by a few per cent on structured files.

See Sections 7 and 8 of the paper for the full list.

## Authorship

The design, implementation and experiments were produced in a working session
with Claude (Anthropic), an AI assistant, directed by the repository owner.

## License

MIT. See [LICENSE](LICENSE).
