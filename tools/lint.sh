#!/usr/bin/env bash
# Runs clang-tidy over the tree. Needs a configured build directory for its
# compile commands: cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
#
#   tools/lint.sh                    every translation unit
#   tools/lint.sh --fix              apply what it can fix
#   SC2M_LINT_FILES="a.cpp b.cpp" tools/lint.sh    just those
#
# One TU costs ~18 seconds here, almost all of it entt instantiation, so the
# whole tree is minutes and the run is parallel by default. SC2M_LINT_JOBS
# overrides the width.
set -euo pipefail
cd "$(dirname "$0")/.."
source tools/clang-env.sh
sc2m_check_clang_version clang-tidy "$SC2M_CLANG_TIDY_VERSION"
build=${SC2M_BUILD_DIR:-build}
[ -f "$build/compile_commands.json" ] || {
	echo "no $build/compile_commands.json -- configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON" >&2
	exit 1
}

files=${SC2M_LINT_FILES:-$(git ls-files 'src/*.cpp' 'tests/*.cpp')}
[ -z "$files" ] && exit 0

jobs=${SC2M_LINT_JOBS:-$(nproc 2> /dev/null || echo 4)}
args=$(sc2m_tidy_args)

# Warnings are the whole point of running this, so they fail the run; without
# it clang-tidy reports and exits 0, and the check is decorative.
#
# Buffered so the status survives the filter: --quiet still emits a
# "N warnings generated" line counting what it suppressed in headers, which
# is noise in a hook and nothing anyone can act on.
sc2m_lint_one() {
	local out status
	out=$(clang-tidy -p "$SC2M_LINT_BUILD" $SC2M_LINT_ARGS \
		${SC2M_LINT_EXTRA:-} "$1" --quiet --warnings-as-errors='*' 2>&1)
	status=$?
	printf '%s\n' "$out" | grep -vE '^[0-9]+ warnings? generated\.$' >&2 || true
	return $status
}
export -f sc2m_lint_one
export SC2M_LINT_BUILD="$build" SC2M_LINT_ARGS="$args"
export SC2M_LINT_EXTRA="${*:-}"

printf '%s\n' $files \
	| xargs -P "$jobs" -I{} bash -c 'sc2m_lint_one "$@"' _ {}
