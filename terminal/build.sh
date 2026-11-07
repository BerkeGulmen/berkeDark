#!/bin/sh
# Builds the interpreter. "./build.sh install" also puts it in ~/.local/bin.
set -e
cd "$(dirname "$0")"
mkdir -p build
clang -std=gnu17 -O2 -Wall -Wextra -o build/berkeDark src/berkeDark.c
if [ "$1" = install ]; then
    install -Dm755 build/berkeDark "$HOME/.local/bin/berkeDark"
fi
