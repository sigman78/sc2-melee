#!/usr/bin/env bash
# Runs clang-tidy over the tree. Needs a configured build directory for its
# compile commands: cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
#
#   tools/lint.sh            report
#   tools/lint.sh --fix      apply what it can fix
set -euo pipefail
cd "$(dirname "$0")/.."
source tools/clang-env.sh
sc2m_check_clang_version clang-tidy "$SC2M_CLANG_TIDY_VERSION"
build=${SC2M_BUILD_DIR:-build}
[ -f "$build/compile_commands.json" ] || {
	echo "no $build/compile_commands.json -- configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON" >&2
	exit 1
}
args=$(sc2m_tidy_args)
status=0
for f in $(git ls-files 'src/*.cpp' 'tests/*.cpp'); do
	clang-tidy -p "$build" $args "$@" "$f" --quiet || status=1
done
exit $status
