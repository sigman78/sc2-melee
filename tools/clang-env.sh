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
