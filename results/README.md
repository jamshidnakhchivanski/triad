# Raw results

Every number in `PAPER.md` comes from these files. `python3 bench/make_tables.py`
turns them into the paper's tables.

| File | Content |
|---|---|
| `silesia.csv` | Silesia corpus: 8 reference settings and TRIAD `-1`, `-2`, `-3` on 12 files |
| `synthetic.csv` | the ten synthetic files; PPMd is missing for two files on which its Python binding failed the round trip |
| `ablation.csv` | TRIAD `-2` with the program search only (`-G`), the expression search only (`-P`), neither (`-1`) and both |
| `seeds.csv` | TRIAD `-2` with seeds 1–4 and the default seed on six Silesia files |
| `synthetic_sha256.txt` | SHA-256 of the synthetic files used |
| `logs/` | the `-v` output of every TRIAD run: repeat distances, each accepted search step, and the final model |

Columns: `file`, `method`, `orig` (bytes), `size` (compressed bytes), `ctime` and
`dtime` (wall-clock seconds for compression and decompression), `roundtrip`
(`ok` means the archive was decompressed and matched the original byte for byte).

Times were taken on a shared 2-vCPU virtual machine with two jobs running at
once; treat them as indicative. Sizes are exact and reproducible.

Log names: `logs/silesia/<file>.<level>.log`, `logs/synthetic/<file>.<level>.log`,
`logs/ablation/<file>.progonly.log` and `.expronly.log`, `logs/seeds/<file>.seed<N>.log`.
