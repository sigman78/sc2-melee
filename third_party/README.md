# third_party

Vendored sources the C++ rewrite builds against, so `src/` and `tests/` have
no build-time dependency on `sc2/`.

| Library | Upstream | Version |
| --- | --- | --- |
| spng | https://github.com/randy408/libspng | 0.7.4 (`spng.c` + `spng.h`, verbatim) |
| miniz | https://github.com/richgel999/miniz | 11.0.2 (`miniz.c` + `miniz.h`, verbatim) |

Both are byte-identical copies of what `sc2/src/libs/vendor/` carries, built
with the same flags — so both trees decode PNG identically by construction,
not by version coincidence. The duplication is deliberate and temporary: it
exists so the rewrite can be extracted into its own repository, after which
each tree keeps the copy it needs.

Fetched rather than vendored: SDL3 and EnTT, via `cmake/Dependencies.cmake`.
These two are vendored instead because they are single-file amalgamations
totalling ~600 KB, and because a PNG decoder is not worth a network
dependency in a build that must work offline.
