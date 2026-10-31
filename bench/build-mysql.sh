#!/usr/bin/env bash
# Builds MySQL from a pinned 8.4 release, with engine/ linked into the
# source tree as storage/celltnx so the celltnx engine builds as a plugin.
# Usage: bench/build-mysql.sh [release|debug|asan|tsan] [ninja targets...]
# MYSQL_ROOT (default ~/.cache/celltnx) holds the source and the build trees;
# each kind builds in $MYSQL_ROOT/build-<kind>.
set -euo pipefail
tag=mysql-8.4.13
kind=${1:-release}
shift || true
repo=$(cd "$(dirname "$0")/.." && pwd)
root=${MYSQL_ROOT:-$HOME/.cache/celltnx}
src=$root/$tag
build=$root/build-$kind

if [ ! -d "$src" ]; then
	mkdir -p "$root"
	curl -fsSL "https://github.com/mysql/mysql-server/archive/refs/tags/$tag.tar.gz" | tar -xz -C "$root"
	mv "$root/mysql-server-$tag" "$src"
fi
if [ -f "$repo/engine/CMakeLists.txt" ]; then
	ln -sfn "$repo/engine" "$src/storage/celltnx"
	# The MTR suite, and the variables MTR derives for the plugin
	# ($CELLTNX_PLUGIN_OPT, $CELLTNX_PLUGIN_LOAD_ADD, ...).
	ln -sfn "$repo/engine/mysql-test" "$src/mysql-test/suite/celltnx"
	grep -q '^ha_celltnx ' "$src/mysql-test/include/plugin.defs" ||
		echo 'ha_celltnx plugin_output_directory no CELLTNX_PLUGIN CELLTNX' >>"$src/mysql-test/include/plugin.defs"
fi

lionturtle=$("$repo/store/lionturtle/fetch.sh")
flags=(
	-G Ninja
	-DCELLTNX_LIONTURTLE_DIR="$lionturtle"
	-DCMAKE_POLICY_VERSION_MINIMUM=3.5 # bundled libraries predate CMake 4
	-DWITH_SSL=system
	-DWITH_UNIT_TESTS=OFF
	-DWITH_ROUTER=OFF
	-DMYSQL_MAINTAINER_MODE=OFF # GCC 15 warns where MySQL's -Werror would fail
)
case $kind in
release) flags+=(-DCMAKE_BUILD_TYPE=RelWithDebInfo) ;;
debug) flags+=(-DWITH_DEBUG=1) ;;
asan) flags+=(-DWITH_DEBUG=1 -DWITH_ASAN=1) ;;
tsan) flags+=(-DWITH_DEBUG=1 -DWITH_TSAN=1) ;;
*)
	echo "unknown build kind $kind" >&2
	exit 2
	;;
esac
cmake -S "$src" -B "$build" "${flags[@]}" >"$build.cmake.log"
cmake --build "$build" -- "$@"
echo "$build"
