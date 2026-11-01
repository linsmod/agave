#!/bin/sh
set -e

src="$(cd "$(dirname "$0")" && pwd)"

if [ "$1" = "release" ]; then
    cmake -S "$src" -B "$src/proj" -DCMAKE_BUILD_TYPE=MinSizeRel
    cmake --build "$src/proj" -j"$(nproc)"
else
    cmake -S "$src" -B "$src/debug" -DCMAKE_BUILD_TYPE=Debug -DOPT_EXAMPLES=ON
    cmake --build "$src/debug" -j"$(nproc)"
fi
