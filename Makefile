CC      ?= cc
CFLAGS  ?= -O3 -Wall -Wextra
# -march=native makes the mixer use the widest SIMD the build machine has.
# Archives stay portable either way: only integer arithmetic decides the stream.
NATIVE  ?= -march=native

all: triad

triad: src/triad.c
	$(CC) $(CFLAGS) $(NATIVE) -o $@ $< -lm

test: triad
	sh tests/run_tests.sh ./triad

clean:
	rm -f triad tests/x86_fuzz
	rm -rf tests/tmp

.PHONY: all test clean
