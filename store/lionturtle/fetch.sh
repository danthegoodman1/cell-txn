#!/usr/bin/env bash
# Fetches lionturtle at the pinned commit and builds its C library. Prints
# the checkout directory: the library is target/release/liblionturtle_c.a and
# the header lionturtle-c/include/lionturtle.h.
# MYSQL_ROOT (default ~/.cache/celltnx) holds the checkout.
set -euo pipefail
# lionturtle v0.5.3.
commit=c5d937073b36e078b2a1e79ea5eee218e77b3605
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
