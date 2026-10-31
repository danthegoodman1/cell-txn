#!/usr/bin/env bash
# Fetches lionturtle at the pinned commit and builds its C library. Prints
# the checkout directory: the library is target/release/liblionturtle_c.a and
# the header lionturtle-c/include/lionturtle.h.
# MYSQL_ROOT (default ~/.cache/celltnx) holds the checkout.
set -euo pipefail
# lionturtle main with the preallocate_log option (PR #13), which no release
# tag carries yet.
commit=5077607daa24d523b3ed4980efaa2cc835ef8779
root=${MYSQL_ROOT:-$HOME/.cache/celltnx}
dir=$root/lionturtle-${commit:0:12}
if [ ! -d "$dir" ]; then
	git init -q "$dir.tmp"
	git -C "$dir.tmp" fetch -q --depth 1 https://github.com/danthegoodman1/lionturtle.git "$commit"
	git -C "$dir.tmp" checkout -q FETCH_HEAD
	mv "$dir.tmp" "$dir"
fi
(cd "$dir" && cargo build --release -q -p lionturtle-c) >&2
echo "$dir"
