#!/bin/sh
# Fetch the Silesia corpus (12 files, 211,938,580 bytes) into bench/corpus/.
set -e
cd "$(dirname "$0")"
mkdir -p corpus
if [ ! -d silesia-src ]; then git clone --depth 1 https://github.com/MiloszKrajewski/SilesiaCorpus silesia-src; fi
for z in silesia-src/*.zip; do unzip -oq "$z" -d corpus; done
ls -l corpus
