/*
 * Fuzz test for the x86 step of the transform program.
 *
 * The step rewrites the 32-bit operand that follows an E8/E9 opcode.  Operands
 * can overlap (an operand byte can itself be E8, or be the 00/FF byte another
 * candidate is tested on), so the order in which positions are visited decides
 * whether the step can be undone.  This test draws bytes from a tiny alphabet
 * that makes such overlaps constant and checks decode(encode(x)) == x.
 *
 * It includes src/triad.c directly so that it tests the shipped function.
 */
#define main triad_main
#include "../src/triad.c"
#undef main

int main(void)
{
    static const U8 al[6] = { 0xE8, 0xE9, 0x00, 0xFF, 0x01, 0x7F };
    long bad = 0, tests = 2000000;
    srand(12345);
    for (long t = 0; t < tests; t++) {
        size_t n = (size_t)(rand() % 40);
        U8 a[64], c[64];
        for (size_t i = 0; i < n; i++) a[i] = (rand() % 8 == 0) ? (U8)rand() : al[rand() % 6];
        memcpy(c, a, n);
        op_x86(c, n, 0);
        op_x86(c, n, 1);
        if (memcmp(a, c, n)) bad++;
    }
    /* typed delta with random parameters */
    long dbad = 0, dtests = 200000;
    for (long t = 0; t < dtests; t++) {
        size_t n = (size_t)(rand() % 200);
        U8 a[256], c[256];
        for (size_t i = 0; i < n; i++) a[i] = (U8)rand();
        Op o = { OP_DELTA, (U8)(1 << (rand() % 3)), (U8)(rand() % 2), (U8)(rand() % 5), (U16)(1 + rand() % 40) };
        memcpy(c, a, n);
        op_delta(c, n, &o, 0);
        op_delta(c, n, &o, 1);
        if (memcmp(a, c, n)) dbad++;
    }
    printf("x86 step:   %ld cases, %ld failures\n", tests, bad);
    printf("delta step: %ld cases, %ld failures\n", dtests, dbad);
    (void)triad_main;
    return bad || dbad;
}
