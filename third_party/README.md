# third_party

Vendored sources the C++ rewrite builds against, so `src/` and `tests/` have
no build-time dependency on `sc2/`.

| Library | Upstream | Version |
| --- | --- | --- |
| spng | https://github.com/randy408/libspng | 0.7.4 (`spng.c` + `spng.h`, verbatim) |
| miniz | https://github.com/richgel999/miniz | 11.0.2 (`miniz.c` + `miniz.h`, verbatim) |

Byte-identical to the copies the sc2-uqm tree vendors, and built with the
same flags — so both decode PNG identically by construction rather than by
version coincidence.

Fetched rather than vendored: SDL3 and EnTT, via `cmake/Dependencies.cmake`.
These two are vendored instead because they are single-file amalgamations
totalling ~600 KB, and because a PNG decoder is not worth a network
dependency in a build that must work offline.
