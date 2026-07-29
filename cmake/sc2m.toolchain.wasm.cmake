# Emscripten toolchain for the `wasm` preset (docs/plan.md Phase 3.1).
#
# This only chains to the toolchain file that ships with emsdk; everything
# UQM-specific lives in the top-level CMakeLists.txt behind if(EMSCRIPTEN).
# Using it directly means `cmake --preset wasm` works without wrapping the
# invocation in `emcmake`.
#
# Requires EMSDK in the environment, which emsdk_env.{sh,bat} sets.

if(DEFINED ENV{EMSDK})
	set(_sc2m_emsdk "$ENV{EMSDK}")
elseif(EXISTS "C:/dev/emsdk/upstream/emscripten")
	# Convenience for the machine this was developed on.
	set(_sc2m_emsdk "C:/dev/emsdk")
endif()

if(NOT _sc2m_emsdk)
	message(FATAL_ERROR
		"The wasm preset needs Emscripten. Install emsdk and run its "
		"emsdk_env script (which sets EMSDK) before configuring:\n"
		"  git clone https://github.com/emscripten-core/emsdk\n"
		"  cd emsdk && ./emsdk install latest && ./emsdk activate latest\n"
		"  source ./emsdk_env.sh   # or emsdk_env.bat on Windows")
endif()

set(_sc2m_em_toolchain
	"${_sc2m_emsdk}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")
if(NOT EXISTS "${_sc2m_em_toolchain}")
	message(FATAL_ERROR
		"Found EMSDK at ${_sc2m_emsdk} but no Emscripten.cmake at "
		"${_sc2m_em_toolchain}. Has `emsdk install` been run?")
endif()

include("${_sc2m_em_toolchain}")

# Shared memory has to be agreed on by every object in the link, including
# the one FetchContent-built dependency left, SDL3 -- wasm-ld rejects the whole
# link if even one was compiled without it. Setting it here rather than on our
# own targets is what makes that global.
# Deliberately no -pthread: this tree is single-threaded, so the build needs
# no SharedArrayBuffer and the page needs no COOP/COEP headers.
