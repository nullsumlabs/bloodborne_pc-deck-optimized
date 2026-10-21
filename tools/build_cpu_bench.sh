#!/usr/bin/env bash
# Builds the CPU benchmark twice, dist/bb-cpu-bench-standard (the portable baseline) and
# dist/bb-cpu-bench-znver2 (-march=znver2 -mtune=znver2, what build_steam_deck.sh targets), so both
# can be copied to the machine being compared and run there:
#
#     bash tools/build_cpu_bench.sh
#     scp dist/bb-cpu-bench-* deck:~
#     ./bb-cpu-bench-standard && ./bb-cpu-bench-znver2
#     python3 tools/cpubench.py bb-cpu-bench-standard-*.log bb-cpu-bench-znver2-*.log
#
# Both are static, so the Deck's glibc does not have to match this host's. Nothing else about the
# two compilations differs: same compiler, same -O2, same source. BB_CPU_BENCH_MARCH names another
# target ISA for the second binary (x86-64-v3 runs on both Zen 2 and Intel AVX2 parts).
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
cc=${CC:-$(command -v cc || command -v gcc)}
march=${BB_CPU_BENCH_MARCH:-znver2}

# XXH3 is header-only with XXH_INLINE_ALL: when a header is available the port's own hash (the
# pipeline and shader caches) is part of the comparison, otherwise that case is left out.
xxhash=()
if [[ -n ${BB_XXHASH_INCLUDE:-} ]]; then
    xxhash=(-I"$BB_XXHASH_INCLUDE" -DHAVE_XXHASH=1)
elif pkg-config --exists libxxhash 2>/dev/null; then
    read -r -a xxhash <<< "$(pkg-config --cflags libxxhash) -DHAVE_XXHASH=1"
elif [[ -f ../deps/prefix/usr/include/xxhash.h ]]; then
    xxhash=(-I../deps/prefix/usr/include -DHAVE_XXHASH=1)
fi

mkdir -p dist
build() {
    local tag=$1 flags=$2 name=dist/bb-cpu-bench-$1
    # shellcheck disable=SC2086
    "$cc" -std=c11 -O2 -static -fno-pie -no-pie -Wall -Wextra -Werror \
        ${flags} "${xxhash[@]}" \
        -DBUILD_TAG="\"$tag\"" -DBUILD_FLAGS="\"-O2 ${flags:-no -march}\"" \
        tools/cpu_bench.c -lm -o "$name"
    echo "$name: $(stat -c %s "$name") bytes"
}

build standard ""
build "$march" "-march=$march -mtune=$march"
"${cc}" --version | head -1
