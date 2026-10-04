/*
 * TRIAD - a lossless compressor built from three layers
 *
 *   1. PROCEDURAL  The archive carries a small program of reversible steps
 *                  (x86 address fix-up, typed delta).  The decoder runs the
 *                  inverse steps in reverse order.
 *   2. FUNCTIONAL  Every statistical context is a pure function
 *                  History -> integer, written as an expression over a few
 *                  combinators (ORD, B, COL, WORD, WORD2, UP, GRAD, AVG).
 *                  The expressions are data: they are stored in the archive
 *                  and interpreted by the decoder.
 *   3. ITERATIVE   The encoder has no fixed model.  It repeatedly changes the
 *                  program and the expressions, measures the real coding cost
 *                  on a sample of the file, and keeps what helps.
 *
 * The predictions of all contexts are combined by an online-trained mixer and
 * coded with a binary arithmetic coder (context mixing, PAQ family).
 *
 * Build:  cc -O3 -march=native -o triad triad.c -lm
 * Use:    triad c [-1|-2|-3] [-v] [-mN] [-sN] [-P] [-G] in out      triad d in out
 *
 * Only integer arithmetic decides the coded stream, so archives are portable
 * between machines.  Floating point is used in the encoder's search only.
 * Assumes '>>' on a negative int is an arithmetic shift (true for gcc, clang,
 * msvc).  The whole file is held in memory; the limit is 4 GB.
 *
 * Cost: about 0.5 MB/s in both directions and up to 1.8 GB of memory at the
 * default -m22.  This is a strong, slow compressor, not a replacement for zstd.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

typedef uint8_t U8; typedef uint16_t U16; typedef uint32_t U32; typedef uint64_t U64;

static void die(const char *m) { fprintf(stderr, "triad: %s\n", m); exit(1); }

static void *xalloc(size_t n)
{
    void *p = NULL;
    if (n == 0) n = 64;
    if (posix_memalign(&p, 64, (n + 63) & ~(size_t)63)) die("out of memory");
    memset(p, 0, n);
    return p;
}

static U32 crc_tab[256];
static U32 crc32(const U8 *b, size_t n)
{
    U32 c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = crc_tab[(c ^ b[i]) & 255] ^ (c >> 8);
    return ~c;
}

/* =====================================================================
 * LAYER 1 - PROCEDURAL: a program of reversible steps
 * ===================================================================== */
enum { OP_X86 = 1, OP_DELTA = 2 };
typedef struct { U8 kind, width, big, offset; U16 stride; } Op;
#define MAXOPS 4
typedef struct { int n; Op op[MAXOPS]; } Program;

/* x86 CALL/JMP: relative target -> absolute, so repeated calls look alike.
 * Operands can overlap, so the encoder walks backwards and the decoder forwards:
 * each side then sees every byte in the same state when it decides. */
static inline void x86_fix(U8 *b, size_t i, int inverse)
{
    if ((b[i] & 0xFE) != 0xE8 || (b[i + 4] != 0x00 && b[i + 4] != 0xFF)) return;
    U32 a = (U32)b[i + 1] | (U32)b[i + 2] << 8 | (U32)b[i + 3] << 16 | (U32)b[i + 4] << 24;
    U32 u = inverse ? a - (U32)i : a + (U32)i;
    u &= 0x1FFFFFF;
    if (u & 0x1000000) u |= 0xFE000000u;                 /* sign-extend 25 bits */
    b[i + 1] = (U8)u; b[i + 2] = (U8)(u >> 8); b[i + 3] = (U8)(u >> 16); b[i + 4] = (U8)(u >> 24);
}
static void op_x86(U8 *b, size_t n, int inverse)
{
    if (n < 5) return;
    if (inverse) for (size_t i = 0; i + 4 < n; i++) x86_fix(b, i, 1);
    else         for (size_t i = n - 4; i-- > 0;) x86_fix(b, i, 0);
}

static inline U32 rd(const U8 *p, int w, int big)
{
    U32 v = 0;
    if (big) for (int k = 0; k < w; k++) v = v << 8 | p[k];
    else     for (int k = w - 1; k >= 0; k--) v = v << 8 | p[k];
    return v;
}
static inline void wr(U8 *p, int w, int big, U32 v)
{
    if (big) for (int k = w - 1; k >= 0; k--) { p[k] = (U8)v; v >>= 8; }
    else     for (int k = 0; k < w; k++)      { p[k] = (U8)v; v >>= 8; }
}

/* typed delta: element[i] -= element[i - stride]; elements are w-byte integers */
static void op_delta(U8 *b, size_t n, const Op *o, int inverse)
{
    size_t w = o->width, s = o->stride, off = o->offset;
    if (n <= off || !w || !s) return;
    U8 *p = b + off;
    size_t cnt = (n - off) / w;
    if (cnt <= s) return;
    if (!inverse)
        for (size_t i = cnt - 1; i >= s; i--)
            wr(p + i * w, (int)w, o->big, rd(p + i * w, (int)w, o->big) - rd(p + (i - s) * w, (int)w, o->big));
    else
        for (size_t i = s; i < cnt; i++)
            wr(p + i * w, (int)w, o->big, rd(p + i * w, (int)w, o->big) + rd(p + (i - s) * w, (int)w, o->big));
}

static void run_op(const Op *o, U8 *b, size_t n, int inverse)
{
    if (o->kind == OP_X86) op_x86(b, n, inverse);
    else if (o->kind == OP_DELTA) op_delta(b, n, o, inverse);
}
static void run_program(const Program *pr, U8 *b, size_t n, int inverse)
{
    for (int k = 0; k < pr->n; k++) run_op(&pr->op[inverse ? pr->n - 1 - k : k], b, n, inverse);
}
static void op_print(FILE *f, const Op *o)
{
    if (o->kind == OP_X86) fprintf(f, "X86");
    else fprintf(f, "DELTA(%d-byte %s, offset %d, stride %d)", o->width, o->big ? "BE" : "LE", o->offset, o->stride);
}

/* =====================================================================
 * LAYER 2 - FUNCTIONAL: contexts as expressions over the history
 * ===================================================================== */
enum { A_ORD = 1, A_BYTE, A_COL, A_WORD, A_WORD2, A_UP, A_GRAD, A_AVG, A_TYPES };
typedef struct { U8 type, b; U16 a, c; } Atom;       /* a, c: distances; b: mask index */
#define MAXATOM 4
#ifndef MAXSLOT
#define MAXSLOT 14
#endif
#define NCORE 6                  /* ORD1..ORD6: the backbone, replaced only for a clear gain */
#define MAXORD 24
typedef struct { U8 n, lim; Atom at[MAXATOM]; } Expr; /* lim: how fast the slot forgets */
typedef struct {
    int n; Expr ex[MAXSLOT];     /* predicting contexts */
    Expr sel;                    /* chooses the mixer weight set */
    Expr apm;                    /* context of the final correction stage */
    U8 minlen;                   /* match model: bytes that must agree */
} Genome;

typedef struct {                 /* everything a context may look at */
    U8 *buf; U32 pos;            /* bytes known so far */
    U32 word, word2;             /* hash of current / previous word */
    U32 line, pline;             /* start of current / previous line */
    U32 ordh[MAXORD + 1];        /* hash of the last k bytes */
    int maxord;
} Hist;

static const U8 MASKS[8] = { 0xFF, 0xF0, 0xE0, 0xC0, 0x80, 0xDF, 0x0F, 0xFC };

static inline U32 hmix(U32 h, U32 v) { h = (h ^ v) * 0x9E3779B1u; return h ^ (h >> 15); }
static inline U32 hbyte(const Hist *h, U32 d) { return d <= h->pos ? h->buf[h->pos - d] : 0; }

static inline U32 atom_eval(Atom a, const Hist *h)
{
    switch (a.type) {
    case A_ORD:   return h->ordh[a.a];
    case A_BYTE:  return a.a <= h->pos ? (U32)(h->buf[h->pos - a.a] & MASKS[a.b]) : 256;
    case A_COL:   { U32 c = a.a ? h->pos % a.a : h->pos - h->line; return c > 65535 ? 65535 : c; }
    case A_WORD:  return h->word;
    case A_WORD2: return h->word2;
    case A_UP:    { U32 i = h->pline + (h->pos - h->line) + a.a; return i < h->line ? h->buf[i] : 256; }
    case A_GRAD:  return (hbyte(h, a.a) + hbyte(h, a.c) - hbyte(h, (U32)a.a + a.c)) & MASKS[a.b];
    case A_AVG:   return ((hbyte(h, a.a) + hbyte(h, a.c)) >> 1) & MASKS[a.b];
    }
    return 0;
}

/* an expression is a fold of its atoms */
static inline U32 expr_eval(const Expr *e, const Hist *h, U32 salt)
{
    U32 x = salt;
    for (int k = 0; k < e->n; k++) x = hmix(x + e->at[k].type * 0x632BE5ABu, atom_eval(e->at[k], h));
    return x;
}

static void hist_push(Hist *h, int c)      /* called after buf[pos++] = c */
{
    int letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 128;
    if (letter) h->word = hmix(h->word + 0x3D4D51CBu, (U32)(c | (c < 128 ? 32 : 0)));
    else if (h->word) { h->word2 = h->word; h->word = 0; }
    if (c == '\n') { h->pline = h->line; h->line = h->pos; }
    U32 x = 0;
    for (int k = 1; k <= h->maxord; k++) {
        x = (x + (k <= (int)h->pos ? h->buf[h->pos - k] : 0) + 1) * 0x2F0B3A55u;
        x ^= x >> 16;
        h->ordh[k] = x;
    }
}

static int expr_maxord(const Expr *e, int m)
{
    for (int k = 0; k < e->n; k++) if (e->at[k].type == A_ORD && e->at[k].a > m) m = e->at[k].a;
    return m;
}
static int genome_maxord(const Genome *g)
{
    int m = expr_maxord(&g->apm, expr_maxord(&g->sel, 1));
    for (int i = 0; i < g->n; i++) m = expr_maxord(&g->ex[i], m);
    return m;
}

static void expr_print(FILE *f, const Expr *e)
{
    if (!e->n) fprintf(f, "ORD0");
    for (int k = 0; k < e->n; k++) {
        Atom a = e->at[k];
        if (k) fprintf(f, "+");
        switch (a.type) {
        case A_ORD:   fprintf(f, "ORD%d", a.a); break;
        case A_BYTE:  fprintf(f, "B%d", a.a); break;
        case A_COL:   if (a.a) fprintf(f, "COL%d", a.a); else fprintf(f, "LINECOL"); break;
        case A_WORD:  fprintf(f, "WORD"); break;
        case A_WORD2: fprintf(f, "WORD2"); break;
        case A_UP:    fprintf(f, "UP%d", a.a); break;
        case A_GRAD:  fprintf(f, "GRAD(%d,%d)", a.a, a.c); break;
        case A_AVG:   fprintf(f, "AVG(%d,%d)", a.a, a.c); break;
        }
        if (a.b && (a.type == A_BYTE || a.type == A_GRAD || a.type == A_AVG)) fprintf(f, "&%02X", MASKS[a.b]);
    }
    if (e->lim && e->lim != 15) fprintf(f, "/%d", e->lim);
}

static int atom_valid(const Atom *a)
{
    if (a->type < 1 || a->type >= A_TYPES || a->b > 7) return 0;
    if (a->type == A_ORD && (a->a < 1 || a->a > MAXORD)) return 0;
    if ((a->type == A_BYTE || a->type == A_GRAD || a->type == A_AVG) && a->a < 1) return 0;
    if ((a->type == A_GRAD || a->type == A_AVG) && a->c < 1) return 0;
    return 1;
}

/* =====================================================================
 * PREDICTOR: context mixing over the expressions of a genome
 * ===================================================================== */
static int16_t STRETCH[4096], SQUASH[4096];
static U16 QIDX[65536];
static int RT[16], DT[1024];
static float COST[4096];

static int squash_calc(int d)
{
    static const int t[33] = { 1, 2, 3, 6, 10, 16, 27, 45, 73, 120, 194, 310, 488, 747, 1101, 1546, 2047, 2549,
        2994, 3348, 3607, 3785, 3901, 3975, 4022, 4050, 4068, 4079, 4085, 4089, 4092, 4093, 4094 };
    if (d > 2047) return 4095;
    if (d < -2047) return 0;
    int w = d & 127;
    d = (d >> 7) + 16;
    return (t[d] * (128 - w) + t[d + 1] * w + 64) >> 7;
}

static void init_tables(void)
{
    for (U32 i = 0; i < 256; i++) {
        U32 c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_tab[i] = c;
    }
    for (int d = -2048; d < 2048; d++) SQUASH[d + 2048] = (int16_t)squash_calc(d);
    int pi = 0;
    for (int x = -2047; x <= 2047; x++) {
        int v = squash_calc(x);
        for (int i = pi; i <= v; i++) STRETCH[i] = (int16_t)x;
        pi = v + 1;
    }
    for (int i = pi; i < 4096; i++) STRETCH[i] = 2047;
    for (int e = 0; e < 65536; e++) QIDX[e] = (U16)(((e & 15) << 5) | ((STRETCH[e >> 4] + 2048) >> 7));
    for (int i = 0; i < 16; i++) RT[i] = 131072 / (2 * i + 3);
    for (int i = 0; i < 1024; i++) DT[i] = 16384 / (i + i + 3);
    for (int i = 1; i < 4096; i++) COST[i] = (float)(-log2(i / 4096.0));
    COST[0] = 12;
}
static inline int squash(int d) { return d > 2047 ? 4095 : d < -2047 ? 0 : SQUASH[d + 2048]; }

#define NSEL 3
#define MAXIN 16                 /* slots + match + bias, padded for SIMD */
#define SELBITS 10
#define NSETS (256 + (1 << SELBITS) + 256)

typedef struct {
    Genome g; int ns, tb, nalloc;
    U16 *tab[MAXSLOT], *bp[MAXSLOT]; U32 hs[MAXSLOT]; int lim[MAXSLOT];
    U32 *cal[MAXSLOT]; int ci[MAXSLOT];           /* counter state -> calibrated probability */
    U32 *mt; int mtb, minlen; U32 mptr, mlen; int mmiss, mi, lenq; U32 msm[64];
    int x[MAXIN] __attribute__((aligned(32)));
    int *w, *wsel[NSEL], o[NSEL], pr[NSEL], wf[NSEL * 2], pf, wfi;
    U32 selv, apmv;
    U16 *apm[3]; int ai[3];
    U32 c0, c4; int bitpos, j;
    Hist h;
} Model;

static void model_alloc(Model *m, int tb, int mtb, int nslots)
{
    memset(m, 0, sizeof *m);
    m->tb = tb; m->mtb = mtb; m->nalloc = nslots;
    for (int i = 0; i < nslots; i++) { m->tab[i] = xalloc(((size_t)32) << tb); m->cal[i] = xalloc(512 * 4); }
    m->mt = xalloc(((size_t)4) << mtb);
    m->w = xalloc(sizeof(int) * NSETS * MAXIN);
    m->apm[0] = xalloc(2 * 256 * 33); m->apm[1] = xalloc(2 * 65536 * 33); m->apm[2] = xalloc(2 * 65536 * 33);
}
static void model_free(Model *m)
{
    for (int i = 0; i < m->nalloc; i++) { free(m->tab[i]); free(m->cal[i]); }
    free(m->mt); free(m->w);
    for (int i = 0; i < 3; i++) free(m->apm[i]);
}

/* 16 counters (one nibble of the next byte) share a 32-byte bucket with a check word */
static inline U16 *bucket(Model *m, int i, U32 h)
{
    U32 idx = (h * 0x9E3779B1u) >> (32 - m->tb);
    U16 chk = (U16)((h * 0xD6E8FEB9u) >> 16);
    if (!chk) chk = 1;
    U16 *b = m->tab[i] + ((size_t)idx << 4), *b2 = m->tab[i] + ((size_t)(idx ^ 1) << 4);
    if (b[0] == chk) return b;
    if (b2[0] == chk) return b2;
    if ((b[1] & 15) > (b2[1] & 15)) b = b2;        /* evict the less used one */
    b[0] = chk;
    for (int k = 1; k < 16; k++) b[k] = 2048 << 4;
    return b;
}
static inline void bucket_prefetch(Model *m, int i, U32 h)
{
    __builtin_prefetch(m->tab[i] + ((size_t)((h * 0x9E3779B1u) >> (32 - m->tb)) << 4), 1);
}

static void model_contexts(Model *m)
{
    for (int i = 0; i < m->ns; i++) {
        m->hs[i] = expr_eval(&m->g.ex[i], &m->h, (U32)i * 0x1000193u + 7);
        bucket_prefetch(m, i, m->hs[i]);
    }
    for (int i = 0; i < m->ns; i++) m->bp[i] = bucket(m, i, m->hs[i]);
    m->selv = (expr_eval(&m->g.sel, &m->h, 0x5bd1e995u) * 0x9E3779B1u) >> (32 - SELBITS);
    m->apmv = (expr_eval(&m->g.apm, &m->h, 0x1b873593u) * 0x9E3779B1u) >> 16;
}

static void model_reset(Model *m, const Genome *g, U8 *buf)
{
    m->g = *g; m->ns = g->n;
    if (g->n > m->nalloc) die("internal: too many slots");
    for (int i = 0; i < m->ns; i++) {
        memset(m->tab[i], 0, ((size_t)32) << m->tb);
        for (int k = 0; k < 512; k++) m->cal[i][k] = ((U32)squash(((k & 31) << 7) - 2048 + 64) << 20) | 2;
        m->lim[i] = g->ex[i].lim ? g->ex[i].lim : 15;
    }
    memset(m->mt, 0, ((size_t)4) << m->mtb);
    for (int k = 0; k < 64; k++) m->msm[k] = (k & 1 ? 0xC0000000u : 0x40000000u);
    memset(m->x, 0, sizeof m->x);
    for (int k = 0; k < NSETS * MAXIN; k++) m->w[k] = 15000;
    for (int k = 0; k < NSEL * 2; k++) m->wf[k] = 65536 / NSEL;
    for (int a = 0; a < 3; a++) {
        int nc = a ? 65536 : 256;
        for (int c = 0; c < nc; c++)
            for (int k = 0; k < 33; k++) m->apm[a][c * 33 + k] = (U16)(squash((k - 16) * 128) * 16);
    }
    memset(&m->h, 0, sizeof m->h);
    m->h.buf = buf; m->h.maxord = genome_maxord(g);
    hist_push(&m->h, 0);
    m->h.word = m->h.word2 = 0;
    m->minlen = g->minlen < 2 ? 2 : g->minlen > 32 ? 32 : g->minlen;
    m->c0 = 1; m->c4 = 0; m->bitpos = 0; m->j = 1;
    m->mlen = 0; m->mptr = 0; m->mmiss = 0; m->lenq = 0;
    model_contexts(m);
}

static inline int mix_dot(const int *restrict x, const int *restrict w)
{
    int d = 0;
    for (int i = 0; i < MAXIN; i++) d += (x[i] * w[i]) >> 4;
    return d >> 12;
}
static inline void mix_train(const int *restrict x, int *restrict w, int err)
{
    for (int i = 0; i < MAXIN; i++) {
        int v = w[i] + ((x[i] * err + 0x8000) >> 16);
        w[i] = v > 524287 ? 524287 : v < -524287 ? -524287 : v;
    }
}
static inline int apm_pp(Model *m, int a, int p, U32 cx)
{
    int s = STRETCH[p] + 2048, lo = s >> 7, wt = s & 127;
    U16 *t = m->apm[a] + cx * 33 + lo;
    m->ai[a] = (int)(cx * 33 + lo + (wt >> 6));
    return (t[0] * (128 - wt) + t[1] * wt) >> 11;
}

/* probability (12 bit) that the next bit is 1 */
static int predict(Model *m)
{
    int n = m->ns, *x = m->x, j = m->j;
    for (int i = 0; i < n; i++) {
        int idx = QIDX[m->bp[i][j]];
        m->ci[i] = idx;
        x[i] = STRETCH[m->cal[i][idx] >> 20];
    }
    m->mi = -1; x[n] = 0;
    if (m->mlen && !m->mmiss) {
        int pb = m->h.buf[m->mptr] | 256;
        if ((U32)(pb >> (8 - m->bitpos)) == m->c0) {
            m->mi = m->lenq * 2 + ((pb >> (7 - m->bitpos)) & 1);
            x[n] = STRETCH[m->msm[m->mi] >> 20];
        } else m->mmiss = 1;
    }
    x[n + 1] = 256;

    int sel[NSEL] = { (int)m->c0, 256 + (int)m->selv, 256 + (1 << SELBITS) + (m->mi < 0 ? 0 : m->lenq) * 8 + m->bitpos };
    int64_t f = 0;
    m->wfi = m->mi < 0 ? 0 : NSEL;
    for (int s = 0; s < NSEL; s++) {
        int *w = m->w + (size_t)sel[s] * MAXIN;
        m->wsel[s] = w;
        int o = mix_dot(x, w);
        if (o > 2047) o = 2047;
        if (o < -2047) o = -2047;
        m->o[s] = o; m->pr[s] = squash(o);
        f += (int64_t)o * m->wf[m->wfi + s];
    }
    m->pf = squash((int)(f >> 16));
    int p = m->pf;
    if (p < 1) p = 1;
    if (p > 4095) p = 4095;
    int a0 = apm_pp(m, 0, p, m->c0);
    int a1 = apm_pp(m, 1, p, m->c0 | (m->c4 & 255) << 8);
    int a2 = apm_pp(m, 2, p, (m->c0 ^ m->apmv) & 0xFFFF);
    p = (2 * p + a0 + 2 * a1 + 3 * a2 + 4) >> 3;
    if (p < 1) p = 1;
    if (p > 4095) p = 4095;
    return p;
}

static inline void sm_update(U32 *t, int y, int limit)
{
    U32 v = *t;
    int n = v & 1023, p = (int)(v >> 10);
    if (n < limit) v++;
    v += (U32)((int64_t)(((y << 22) - p) >> 3) * DT[n]) & 0xFFFFFC00u;
    *t = v;
}

static void update(Model *m, int y)
{
    int n = m->ns, j = m->j;
    for (int i = 0; i < n; i++) {
        U16 *e = &m->bp[i][j];
        int pr = *e >> 4, c = *e & 15;
        pr += (((y << 12) - pr) * RT[c]) >> 16;
        if (c < m->lim[i]) c++;
        *e = (U16)(pr << 4 | c);
        sm_update(&m->cal[i][m->ci[i]], y, 255);
    }
    if (m->mi >= 0) {
        sm_update(&m->msm[m->mi], y, 1023);
        if ((m->mi & 1) != y) m->mmiss = 1;
    }
    for (int s = 0; s < NSEL; s++) mix_train(m->x, m->wsel[s], ((y << 12) - m->pr[s]) * 8);
    int err = ((y << 12) - m->pf) * 2;
    for (int s = 0; s < NSEL; s++) m->wf[m->wfi + s] += (m->o[s] * err + 0x8000) >> 16;
    for (int a = 0; a < 3; a++) { U16 *t = &m->apm[a][m->ai[a]]; *t += ((y ? 65535 : 0) - *t) >> 6; }

    m->c0 = m->c0 * 2 + y; m->j = j * 2 + y; m->bitpos++;
    if (m->bitpos == 4) {
        U32 hn[MAXSLOT];
        for (int i = 0; i < n; i++) { hn[i] = hmix(m->hs[i] + 0x51ED27u, m->c0); bucket_prefetch(m, i, hn[i]); }
        for (int i = 0; i < n; i++) m->bp[i] = bucket(m, i, hn[i]);
        m->j = 1;
    } else if (m->bitpos == 8) {
        int c = m->c0 & 255;
        Hist *h = &m->h;
        h->buf[h->pos++] = (U8)c;
        m->c4 = m->c4 << 8 | c;
        hist_push(h, c);
        m->c0 = 1; m->bitpos = 0; m->j = 1;
        /* match model: follow the most recent earlier occurrence of the last bytes */
        U32 pos = h->pos;
        if (m->mlen && !m->mmiss) { if (m->mlen < 65535) m->mlen++; m->mptr++; }
        else m->mlen = 0;
        if (pos >= (U32)m->minlen) {
            U32 hh = 0;
            for (int k = 1; k <= m->minlen; k++) hh = (hh + h->buf[pos - k] + 1) * 0x2F0B3A55u;
            hh = (hh * 0x9E3779B1u) >> (32 - m->mtb);
            if (!m->mlen) {
                U32 p = m->mt[hh];
                if (p) {
                    U32 l = 0;
                    while (l < 32 && l < p && h->buf[p - 1 - l] == h->buf[pos - 1 - l]) l++;
                    m->mlen = l; m->mptr = p;
                }
            }
            m->mt[hh] = pos;
        }
        m->mmiss = 0;
        m->lenq = m->mlen < 16 ? (int)m->mlen : 16 + (int)((m->mlen - 16) >> 3);
        if (m->lenq > 31) m->lenq = 31;
        model_contexts(m);
    }
}

/* =====================================================================
 * LAYER 3 - ITERATIVE: search for the program and the expressions
 * ===================================================================== */
#define NCHK 8
#ifndef SEGLEN
#define SEGLEN 65536            /* the search sample: SEGCNT pieces of SEGLEN bytes, spread over the file */
#define SEGCNT 4
#endif
static U32 rng_s = 0x2545F491u;      /* default seed; -sN replaces it */
static int opt_no_program, opt_no_expr;   /* ablation switches: -P, -G */
static U32 rnd(void) { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 17; rng_s ^= rng_s << 5; return rng_s; }

typedef struct {
    Model m; U8 *scratch; const U8 *smp; U32 n;
    double best, base, chk[NCHK]; long evals;
    int strides[12], nstr, verbose;
} Search;

/* real coding cost (bits) of the sample under genome g; gives up early when hopeless */
static double eval_cost(Search *s, const Genome *g, double tmp[NCHK], int may_abort)
{
    Model *m = &s->m;
    model_reset(m, g, s->scratch);
    double cost = 0;
    U32 n = s->n, step = n / NCHK + 1;
    int ck = 0;
    s->evals++;
    for (U32 i = 0; i < n; i++) {
        int c = s->smp[i];
        for (int b = 7; b >= 0; b--) {
            int y = (c >> b) & 1, p = predict(m);
            cost += COST[y ? p : 4096 - p];
            update(m, y);
        }
        if ((i + 1) % step == 0 && ck < NCHK) {
            tmp[ck] = cost;
            if (may_abort && cost > s->chk[ck] * 1.015 + 64) return 1e300;
            ck++;
        }
    }
    for (; ck < NCHK; ck++) tmp[ck] = cost;
    return cost;
}

static void default_genome(Genome *g)
{
    static const Expr d[MAXSLOT] = {
        {1, 15, {{A_ORD, 0, 1, 0}}}, {1, 15, {{A_ORD, 0, 2, 0}}}, {1, 15, {{A_ORD, 0, 3, 0}}},
        {1, 15, {{A_ORD, 0, 4, 0}}}, {1, 15, {{A_ORD, 0, 5, 0}}}, {1, 15, {{A_ORD, 0, 6, 0}}},
        {1, 15, {{A_ORD, 0, 8, 0}}}, {1, 15, {{A_WORD, 0, 0, 0}}},
        {2, 15, {{A_WORD, 0, 0, 0}, {A_WORD2, 0, 0, 0}}},
        {2, 15, {{A_BYTE, 0, 2, 0}, {A_BYTE, 0, 3, 0}}},
        {2, 15, {{A_BYTE, 0, 1, 0}, {A_BYTE, 0, 3, 0}}}, {0, 15, {{0, 0, 0, 0}}},
#if MAXSLOT > 12
        {2, 15, {{A_BYTE, 0, 1, 0}, {A_WORD2, 0, 0, 0}}}, {1, 15, {{A_ORD, 0, 12, 0}}},
#endif
    };
    static const Expr sel = {1, 15, {{A_BYTE, 0, 1, 0}}}, apm = {1, 15, {{A_ORD, 0, 2, 0}}};
    memset(g, 0, sizeof *g);
    g->n = MAXSLOT; memcpy(g->ex, d, sizeof d);
    g->sel = sel; g->apm = apm; g->minlen = 6;
}

static U16 rand_dist(Search *s)
{
    if (s->nstr && rnd() % 2) {
        int st = s->strides[rnd() % s->nstr];
        int v = st * (1 + (int)(rnd() % 4 == 0)) + (rnd() % 5 < 2 ? (int)(rnd() % 3) - 1 : 0);
        return (U16)(v < 1 ? 1 : v > 65535 ? 65535 : v);
    }
    return (U16)(1 + rnd() % 8);
}
static Atom rand_atom(Search *s)
{
    static const U16 ords[] = { 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 24 };
    static const U16 cols[] = { 0, 2, 4, 8 };
    Atom a = { 0, 0, 0, 0 };
    U32 r = rnd() % 100;
    if (r < 20) { a.type = A_ORD; a.a = ords[rnd() % 12]; }
    else if (r < 56) { a.type = A_BYTE; a.a = rand_dist(s); a.b = rnd() % 5 < 3 ? 0 : (U8)(rnd() % 8); }
    else if (r < 70) { a.type = A_COL; a.a = (s->nstr && rnd() % 3) ? (U16)s->strides[rnd() % s->nstr] : cols[rnd() % 4]; }
    else if (r < 78) a.type = A_WORD;
    else if (r < 82) a.type = A_WORD2;
    else if (r < 88) { a.type = A_UP; a.a = (U16)(rnd() % 3); }
    else {
        a.type = rnd() % 2 ? A_GRAD : A_AVG;
        a.a = (U16)(1 + rnd() % 4); a.c = rand_dist(s);
        if (a.c == a.a) a.c++;
        a.b = rnd() % 2 ? 0 : (U8)(1 + rnd() % 3);
    }
    return a;
}
static int expr_same(const Expr *a, const Expr *b)
{
    if (a->n != b->n || a->lim != b->lim) return 0;
    for (int k = 0; k < a->n; k++)
        if (a->at[k].type != b->at[k].type || a->at[k].a != b->at[k].a || a->at[k].b != b->at[k].b || a->at[k].c != b->at[k].c) return 0;
    return 1;
}

static int genome_same(const Genome *a, const Genome *b)
{
    if (a->n != b->n || a->minlen != b->minlen || !expr_same(&a->sel, &b->sel) || !expr_same(&a->apm, &b->apm)) return 0;
    for (int i = 0; i < a->n; i++) if (!expr_same(&a->ex[i], &b->ex[i])) return 0;
    return 1;
}

/* one random change; returns 0 when the change is pointless */
static int mutate(Search *s, Genome *g, int *core)
{
    *core = 0;
    static const U8 lims[] = { 3, 5, 7, 10, 15 }, mls[] = { 3, 4, 5, 6, 7, 8, 10, 12, 16, 24, 32 };
    U32 r = rnd() % 100;
    Expr *e; int i = -1;
    if (r < 5) { U8 v = mls[rnd() % 11]; if (v == g->minlen) return 0; g->minlen = v; return 1; }
    if (r < 12) e = &g->sel;
    else if (r < 19) e = &g->apm;
    else { i = (int)(rnd() % g->n); e = &g->ex[i]; }
    Expr old = *e;
    int op = (int)(rnd() % 6);
    *core = i >= 0 && i < NCORE && op != 5 && old.n == 1 && old.at[0].type == A_ORD;
    switch (op) {
    case 0: e->n = (U8)(1 + rnd() % 3); for (int k = 0; k < e->n; k++) e->at[k] = rand_atom(s); break;
    case 1: if (e->n >= MAXATOM) return 0; e->at[e->n++] = rand_atom(s); break;
    case 2: if (e->n < 2) return 0; { int k = (int)(rnd() % e->n); e->at[k] = e->at[--e->n]; } break;
    case 3: if (!e->n) return 0; e->at[rnd() % e->n] = rand_atom(s); break;
    case 4: { int q = (int)(rnd() % g->n); if (q == i || g->ex[q].n >= MAXATOM) return 0; *e = g->ex[q]; e->at[e->n++] = rand_atom(s); } break;
    case 5: if (i < 0) return 0; e->lim = lims[rnd() % 5]; break;
    }
    if (i < 0) e->lim = 15;
    if (expr_same(e, &old)) return 0;
    if (i >= 0) for (int q = 0; q < g->n; q++) if (q != i && expr_same(e, &g->ex[q])) return 0;
    return 1;
}

/* record widths: distances at which bytes repeat or stay numerically close */
static void find_strides(Search *s, const U8 *b, U32 n)
{
    U32 lim = n > 131072 ? 131072 : n, pmax = 8192;
    s->nstr = 0;
    if (lim < 64) return;
    if (pmax > lim / 4) pmax = lim / 4;
    double *eq = malloc(sizeof(double) * (pmax + 1)), *ad = malloc(sizeof(double) * (pmax + 1));
    for (U32 p = 1; p <= pmax; p++) {
        U32 e = 0, a = 0;
        for (U32 i = p; i < lim; i++) { int d = b[i] - b[i - p]; e += d == 0; a += (U32)(d < 0 ? -d : d); }
        eq[p] = (double)e / (lim - p); ad[p] = (double)a / (lim - p);
    }
    for (int pass = 0; pass < 2; pass++)
        for (int t = 0; t < 3; t++) {
            U32 bp = 0; double bv = -1e300;
            for (U32 p = 5; p <= pmax; p++) {
                double v = pass ? -ad[p] : eq[p];
                int used = 0;
                for (int k = 0; k < s->nstr; k++) if ((int)p % s->strides[k] == 0) used = 1;   /* skip multiples */
                if (!used && v > bv) { bv = v; bp = p; }
            }
            if (bp && s->nstr < 12) s->strides[s->nstr++] = (int)bp;
        }
    free(eq); free(ad);
}

static double entropy0(const U8 *b, U32 n)
{
    U32 f[256] = { 0 };
    double h = 0;
    for (U32 i = 0; i < n; i++) f[b[i]]++;
    for (int i = 0; i < 256; i++) if (f[i]) h -= f[i] * log2((double)f[i] / n);
    return h;
}

/* phase A: grow the transform program one step at a time while the measured cost falls */
static void search_program(Search *s, const U8 *raw, U8 *work, U32 n, Program *prog, const Genome *g)
{
    double tmp[NCHK];
    prog->n = 0;
    memcpy(work, raw, n);
    s->smp = work; s->n = n;
    s->best = s->base = eval_cost(s, g, s->chk, 0);
    if (s->verbose) fprintf(stderr, "  no program: %.0f bytes on the sample\n", s->best / 8);
    U8 *t = malloc(n ? n : 1);
    for (int depth = 0; depth < (opt_no_program ? 0 : 2); depth++) {
        Op cand[8]; int nc = 0;
        double h0 = entropy0(work, n);
        if (depth == 0) {
            Op o = { OP_X86, 0, 0, 0, 0 };
            U32 hits = 0;
            for (U32 i = 0; i + 4 < n; i++) if ((raw[i] & 0xFE) == 0xE8 && (raw[i + 4] == 0 || raw[i + 4] == 0xFF)) hits++;
            if (hits * 2000 > n) cand[nc++] = o;
        }
        int sl[16], ns = 4;
        sl[0] = 1; sl[1] = 2; sl[2] = 3; sl[3] = 4;
        for (int k = 0; k < s->nstr && ns < 16; k++) sl[ns++] = s->strides[k];
        struct { Op o; double h; } pool[256]; int np = 0;
        for (int w = 1; w <= 4; w *= 2)
            for (int big = 0; big < (w > 1 ? 2 : 1); big++)
                for (int off = 0; off < w; off++)
                    for (int k = 0; k < ns; k++) {
                        if (k >= 4 && sl[k] % w) continue;
                        int st = k >= 4 ? sl[k] / w : sl[k];
                        if (w > 1 && k < 4 && st > 2) continue;
                        Op o = { OP_DELTA, (U8)w, (U8)big, (U8)off, (U16)st };
                        memcpy(t, work, n); op_delta(t, n, &o, 0);
                        double h = entropy0(t, n);
                        if (h < h0 * 0.97 && np < 256) { pool[np].o = o; pool[np].h = h; np++; }
                    }
        for (int a = 0; a < np && a < 4; a++) {      /* only the 4 most promising deltas get a real test */
            int bi = a;
            for (int q = a + 1; q < np; q++) if (pool[q].h < pool[bi].h) bi = q;
            Op so = pool[a].o; double sh = pool[a].h;
            pool[a] = pool[bi]; pool[bi].o = so; pool[bi].h = sh;
            cand[nc++] = pool[a].o;
        }
        int bestc = -1; double bc = s->best * 0.995, bchk[NCHK];
        for (int c = 0; c < nc; c++) {
            memcpy(t, work, n); run_op(&cand[c], t, n, 0);
            s->smp = t;
            double v = eval_cost(s, g, tmp, 0);
            if (s->verbose) { fprintf(stderr, "  try "); op_print(stderr, &cand[c]); fprintf(stderr, ": %.0f\n", v / 8); }
            if (v < bc) { bc = v; bestc = c; memcpy(bchk, tmp, sizeof bchk); }
        }
        s->smp = work;
        if (bestc < 0) break;
        run_op(&cand[bestc], work, n, 0);
        prog->op[prog->n++] = cand[bestc];
        s->best = bc; memcpy(s->chk, bchk, sizeof bchk);
    }
    free(t);
}

/* phase B: hill-climb the expressions */
static void search_genome(Search *s, Genome *g, int iters)
{
    double tmp[NCHK];
    for (int it = 0; it < iters; it++) {
        Genome c = *g;
        int core;
        if (!mutate(s, &c, &core)) continue;
        double v = eval_cost(s, &c, tmp, 1);
        /* a short sample undervalues long contexts, so the backbone needs a clear win */
        if (v >= s->best * (core ? 0.996 : 0.9997)) continue;
        if (s->verbose) {
            fprintf(stderr, "  step %3d: %.0f -> %.0f  ", it, s->best / 8, v / 8);
            for (int i = 0; i < c.n; i++) if (!expr_same(&c.ex[i], &g->ex[i])) { expr_print(stderr, &g->ex[i]); fprintf(stderr, " => "); expr_print(stderr, &c.ex[i]); }
            if (!expr_same(&c.sel, &g->sel)) { fprintf(stderr, "mixer select => "); expr_print(stderr, &c.sel); }
            if (!expr_same(&c.apm, &g->apm)) { fprintf(stderr, "correction => "); expr_print(stderr, &c.apm); }
            if (c.minlen != g->minlen) fprintf(stderr, "match length => %d", c.minlen);
            fprintf(stderr, "\n");
        }
        *g = c; s->best = v; memcpy(s->chk, tmp, sizeof tmp);
    }
}

/* phase C: drop contexts that do not pay for themselves (faster coding) */
static void prune_genome(Search *s, Genome *g)
{
    double tmp[NCHK];
    for (int i = g->n - 1; i >= 0 && g->n > 4; i--) {
        Genome c = *g;
        for (int k = i; k + 1 < c.n; k++) c.ex[k] = c.ex[k + 1];
        c.n--;
        double v = eval_cost(s, &c, tmp, 1);
        int core = i < NCORE && g->ex[i].n == 1 && g->ex[i].at[0].type == A_ORD;
        if (v > s->best * (core ? 1.0 : 1.0003)) continue;
        if (s->verbose) { fprintf(stderr, "  prune "); expr_print(stderr, &g->ex[i]); fprintf(stderr, ": %.0f -> %.0f\n", s->best / 8, v / 8); }
        *g = c;
        if (v < s->best) { s->best = v; memcpy(s->chk, tmp, sizeof tmp); }
    }
}

/* =====================================================================
 * ARITHMETIC CODER and container
 * ===================================================================== */
typedef struct { U8 *p; size_t n, cap; } Buf;
static inline void put(Buf *b, int c)
{
    if (b->n == b->cap) {
        b->cap = b->cap * 2 + 65536;
        b->p = realloc(b->p, b->cap);
        if (!b->p) die("out of memory");
    }
    b->p[b->n++] = (U8)c;
}
static void put_n(Buf *b, U64 v, int k) { for (int i = 0; i < k; i++) put(b, (int)(v >> (8 * i)) & 255); }

static void put_expr(Buf *o, const Expr *e)
{
    put(o, e->n); put(o, e->lim);
    for (int k = 0; k < e->n; k++) { put(o, e->at[k].type); put(o, e->at[k].b); put_n(o, e->at[k].a, 2); put_n(o, e->at[k].c, 2); }
}
static void put_models(Buf *o, const Program *pr, const Genome *g)
{
    put(o, pr->n);
    for (int k = 0; k < pr->n; k++) {
        put(o, pr->op[k].kind); put(o, pr->op[k].width); put(o, pr->op[k].big); put(o, pr->op[k].offset);
        put_n(o, pr->op[k].stride, 2);
    }
    put(o, g->n);
    for (int i = 0; i < g->n; i++) put_expr(o, &g->ex[i]);
    put_expr(o, &g->sel); put_expr(o, &g->apm); put(o, g->minlen);
}
static int table_bits(U64 n, int cap) { int tb = 16; while (tb < cap && ((U64)1 << tb) < 2 * n) tb++; return tb; }

static U8 *read_file(const char *fn, size_t *n)
{
    FILE *f = fopen(fn, "rb");
    if (!f) die("cannot open input");
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) die("cannot read input");
    U8 *b = malloc(sz ? (size_t)sz : 1);
    if (!b) die("out of memory");
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) die("read error");
    fclose(f);
    *n = (size_t)sz;
    return b;
}
static void write_file(const char *fn, const U8 *b, size_t n)
{
    FILE *f = fopen(fn, "wb");
    if (!f) die("cannot open output");
    if (fwrite(b, 1, n, f) != n) die("write error");
    if (fclose(f)) die("write error");
}

#define MAGIC 0x32445254u        /* "TRD2" */

static void compress(const char *in, const char *out, int level, int verbose, int tbcap)
{
    size_t n;
    U8 *buf = read_file(in, &n);
    if (n >= 0xFFFFFFF0u) die("file too large (4 GB limit)");
    U32 crc = crc32(buf, n);
    Program prog = { 0 };
    Genome g;
    default_genome(&g);
    clock_t t0 = clock();

    if (level > 1 && n >= 4096) {
        static const U32 seg_len[] = { 0, 0, SEGLEN, SEGLEN };
        static const int seg_cnt[] = { 0, 0, SEGCNT, SEGCNT }, iters[] = { 0, 0, 150, 600 };
        U32 sl = seg_len[level], total = sl * seg_cnt[level], sn;
        U8 *raw = malloc(total), *work = malloc(total), *scratch = malloc(total);
        if (n <= total) { sn = (U32)n; memcpy(raw, buf, n); }
        else {
            sn = total;
            for (int k = 0; k < seg_cnt[level]; k++) {
                size_t at = (n - sl) / (seg_cnt[level] - 1) * k;
                at -= at % 4096;                 /* keep record/word alignment */
                memcpy(raw + (size_t)k * sl, buf + at, sl);
            }
        }
        Search *s = xalloc(sizeof *s);
        s->verbose = verbose; s->scratch = scratch;
        model_alloc(&s->m, 18, 18, MAXSLOT);
        find_strides(s, raw, sn);
        if (verbose) {
            fprintf(stderr, "sample: %u bytes; repeat distances:", sn);
            for (int k = 0; k < s->nstr; k++) fprintf(stderr, " %d", s->strides[k]);
            fprintf(stderr, "\n");
        }
        search_program(s, raw, work, sn, &prog, &g);
        double base = s->base;
        if (prog.n) find_strides(s, work, sn);
        if (!opt_no_expr) {
            search_genome(s, &g, iters[level]);
            prune_genome(s, &g);
        }
        Buf hd = { 0 };
        put_models(&hd, &prog, &g);
        /* the description of the model must pay for itself */
        if ((base - s->best) / 8 * ((double)n / sn) <= (double)hd.n) { prog.n = 0; default_genome(&g); }
        free(hd.p);
        if (verbose) fprintf(stderr, "search: %ld evaluations, %.1f s\n", s->evals, (double)(clock() - t0) / CLOCKS_PER_SEC);
        model_free(&s->m); free(s); free(raw); free(work); free(scratch);
    }
    if (verbose) {
        fprintf(stderr, "program :");
        if (!prog.n) fprintf(stderr, " (none)");
        for (int k = 0; k < prog.n; k++) { fprintf(stderr, " "); op_print(stderr, &prog.op[k]); }
        fprintf(stderr, "\ncontexts:");
        for (int i = 0; i < g.n; i++) { fprintf(stderr, " "); expr_print(stderr, &g.ex[i]); }
        fprintf(stderr, "\nmixer select: "); expr_print(stderr, &g.sel);
        fprintf(stderr, "   correction: "); expr_print(stderr, &g.apm);
        fprintf(stderr, "   match length: %d\n", g.minlen);
    }

    Buf o = { 0 };
    int tb = table_bits(n, tbcap);
    Genome dg;
    default_genome(&dg);
    int plain = !prog.n && genome_same(&g, &dg);       /* mode 2: default model, nothing to describe */
    put_n(&o, MAGIC, 4); put(&o, plain ? 2 : 1); put_n(&o, n, 8); put_n(&o, crc, 4); put(&o, tb);
    if (!plain) put_models(&o, &prog, &g);
    run_program(&prog, buf, n, 0);

    Model *m = xalloc(sizeof *m);
    model_alloc(m, tb, tb > 22 ? 22 : tb, g.n);
    U8 *hist = malloc(n ? n : 1);
    model_reset(m, &g, hist);
    U32 x1 = 0, x2 = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        int c = buf[i];
        for (int b = 7; b >= 0; b--) {
            int y = (c >> b) & 1, p = predict(m);
            U32 xm = x1 + (U32)(((U64)(x2 - x1) * (U32)p) >> 12);
            if (y) x2 = xm; else x1 = xm + 1;
            update(m, y);
            while (((x1 ^ x2) & 0xFF000000u) == 0) { put(&o, (int)(x1 >> 24)); x1 <<= 8; x2 = x2 << 8 | 255; }
        }
    }
    for (int k = 0; k < 4; k++) { put(&o, (int)(x1 >> 24)); x1 <<= 8; }
    if (o.n >= n + 17) {                               /* incompressible: store as is */
        run_program(&prog, buf, n, 1);
        o.n = 0;
        put_n(&o, MAGIC, 4); put(&o, 0); put_n(&o, n, 8); put_n(&o, crc, 4);
        for (size_t i = 0; i < n; i++) put(&o, buf[i]);
    }
    write_file(out, o.p, o.n);
    model_free(m); free(m); free(hist); free(buf);
    if (verbose) fprintf(stderr, "%zu -> %zu bytes (%.3f bits/byte), %.1f s\n", n, o.n, n ? 8.0 * o.n / n : 0.0, (double)(clock() - t0) / CLOCKS_PER_SEC);
    free(o.p);
}

typedef struct { const U8 *z; size_t n, q; } In;
static inline int get(In *in) { return in->q < in->n ? in->z[in->q++] : 0; }
static void get_expr(In *in, Expr *e)
{
    e->n = (U8)get(in); e->lim = (U8)get(in);
    if (e->n > MAXATOM || e->lim > 15) die("corrupt model");
    for (int k = 0; k < e->n; k++) {
        Atom *a = &e->at[k];
        a->type = (U8)get(in); a->b = (U8)get(in);
        a->a = (U16)get(in); a->a |= (U16)(get(in) << 8);
        a->c = (U16)get(in); a->c |= (U16)(get(in) << 8);
        if (!atom_valid(a)) die("corrupt model");
    }
}

static void decompress(const char *inf, const char *out)
{
    In in = { 0, 0, 0 };
    in.z = read_file(inf, &in.n);
    if (in.n < 17 || get(&in) != 'T' || get(&in) != 'R' || get(&in) != 'D' || get(&in) != '2') die("not a TRIAD file");
    int mode = get(&in);
    U64 n = 0;
    for (int i = 0; i < 8; i++) n |= (U64)get(&in) << (8 * i);
    U32 crc = 0;
    for (int i = 0; i < 4; i++) crc |= (U32)get(&in) << (8 * i);
    if (n >= 0xFFFFFFF0u || mode > 2) die("corrupt header");
    if (mode == 0 && in.n - in.q != n) die("corrupt stored block");
    if (mode != 0 && n / 8 > in.n * 600) die("corrupt header");      /* no real file compresses 4800:1 here */
    U8 *buf = malloc(n ? n : 1);
    if (!buf) die("out of memory");
    if (mode == 0) memcpy(buf, in.z + in.q, n);
    else {
        int tb = get(&in);
        if (tb < 16 || tb > 24) die("corrupt header");
        Program prog; Genome g;
        default_genome(&g);
        prog.n = mode == 1 ? get(&in) : 0;
        if (prog.n > MAXOPS) die("corrupt program");
        for (int k = 0; k < prog.n; k++) {
            Op *o = &prog.op[k];
            o->kind = (U8)get(&in); o->width = (U8)get(&in); o->big = (U8)get(&in); o->offset = (U8)get(&in);
            o->stride = (U16)get(&in); o->stride |= (U16)(get(&in) << 8);
            if (o->kind != OP_X86 && o->kind != OP_DELTA) die("corrupt program");
            if (o->kind == OP_DELTA && (o->width < 1 || o->width > 4 || !o->stride || o->big > 1)) die("corrupt program");
        }
        if (mode == 1) {
            memset(&g, 0, sizeof g);
            g.n = get(&in);
            if (g.n < 1 || g.n > MAXSLOT) die("corrupt model");
            for (int i = 0; i < g.n; i++) get_expr(&in, &g.ex[i]);
            get_expr(&in, &g.sel); get_expr(&in, &g.apm);
            g.minlen = (U8)get(&in);
        }
        Model *m = xalloc(sizeof *m);
        model_alloc(m, tb, tb > 22 ? 22 : tb, g.n);
        model_reset(m, &g, buf);
        U32 x1 = 0, x2 = 0xFFFFFFFFu, x = 0;
        for (int i = 0; i < 4; i++) x = x << 8 | (U32)get(&in);
        for (U64 i = 0; i < n * 8; i++) {
            int p = predict(m);
            U32 xm = x1 + (U32)(((U64)(x2 - x1) * (U32)p) >> 12);
            int y = x <= xm;
            if (y) x2 = xm; else x1 = xm + 1;
            update(m, y);
            while (((x1 ^ x2) & 0xFF000000u) == 0) { x1 <<= 8; x2 = x2 << 8 | 255; x = x << 8 | (U32)get(&in); }
        }
        run_program(&prog, buf, n, 1);
        model_free(m); free(m);
    }
    if (crc32(buf, n) != crc) die("CRC mismatch - archive is damaged");
    write_file(out, buf, n);
    free(buf); free((void *)in.z);
}

int main(int argc, char **argv)
{
    init_tables();
    int level = 2, verbose = 0, tbcap = 22, nf = 0;
    const char *f[2] = { 0, 0 };
    if (argc < 4) {
        fprintf(stderr, "usage: triad c [-1|-2|-3] [-v] [-mN] [-sN] [-P] [-G] in out\n       triad d in out\n"
                        "  -1 fixed model   -2 search (default)   -3 longer search\n"
                        "  -mN table size 2^N buckets per context (16..24, default 22)\n"
                        "  -sN seed of the search (default 625341585)\n"
                        "  -P  do not search for a transform program   -G  do not search for expressions\n");
        return 2;
    }
    for (int i = 2; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] >= '1' && argv[i][1] <= '3' && !argv[i][2]) level = argv[i][1] - '0';
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-P")) opt_no_program = 1;
        else if (!strcmp(argv[i], "-G")) opt_no_expr = 1;
        else if (argv[i][0] == '-' && argv[i][1] == 's') { rng_s = (U32)strtoul(argv[i] + 2, NULL, 10); if (!rng_s) die("-s must be a non-zero number"); }
        else if (argv[i][0] == '-' && argv[i][1] == 'm') { tbcap = atoi(argv[i] + 2); if (tbcap < 16 || tbcap > 24) die("-m must be 16..24"); }
        else if (nf < 2) f[nf++] = argv[i];
    }
    if (nf != 2) die("need an input and an output file");
    if (argv[1][0] == 'c') compress(f[0], f[1], level, verbose, tbcap);
    else if (argv[1][0] == 'd') decompress(f[0], f[1]);
    else die("first argument must be c or d");
    return 0;
}
