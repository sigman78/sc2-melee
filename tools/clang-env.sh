# The versions CI pins, and the ones the tree is formatted with. clang-format's
# output is not stable across LLVM majors, so a mismatch reports violations on
# lines nobody touched -- which is how this cost a green CI run once.
# Match them with: pipx install clang-format==21.1.5 clang-tidy==21.1.6
SC2M_CLANG_FORMAT_VERSION=21.1.5
SC2M_CLANG_TIDY_VERSION=21.1.6

# Warns, never fails: a stale tool still formats almost everything correctly,
# and blocking a commit over a point release would be worse than the drift.
sc2m_check_clang_version() {
	local tool=$1 want=$2 have
	command -v "$tool" > /dev/null || return 0
	have=$("$tool" --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
	[ -z "$have" ] && return 0
	[ "$have" = "$want" ] && return 0
	echo "warning: $tool is $have, the tree is pinned to $want" >&2
	echo "  match it with: pipx install $tool==$want" >&2
}

# clang-tidy uses clang's own header search, which on MinGW finds nothing.
# Hand it GCC's include paths instead. Sourced by the lint scripts and hook.
sc2m_tidy_args() {
	local inc
	inc=$(echo | g++ -x c++ -E -v - 2>&1 \
		| sed -n '/#include <...> search starts here:/,/End of search list/p' \
		| grep '^ ' | sed 's/^ //')
	local args=""
	local d
	for d in $inc; do args="$args --extra-arg=-isystem$d"; done
	echo "$args --extra-arg=--target=x86_64-w64-mingw32"
}
