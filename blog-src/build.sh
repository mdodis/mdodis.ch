#!/bin/bash
# Builds docubuild2 and generates the blog from posts/*.docuspec into ../blog/
set -e
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cd "$SCRIPT_DIR"

mkdir -p build ../blog
gcc -O1 -g -pthread -o build/docubuild2 -I . docubuild2.c -ldl
cp static/* ../blog/

# Run from inside posts/: on Linux the directory walker stats entries relative to the working directory,
# so the source directory must be the current one and contain no subdirectories.
cd posts
../build/docubuild2 . -o ../../blog -t ../templates -p
