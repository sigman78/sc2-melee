# Emscripten toolchain for the `wasm` preset (docs/plan.md Phase 3.1).
#
# This only chains to the toolchain file that ships with emsdk; everything
# UQM-specific lives in the top-level CMakeLists.txt behind if(EMSCRIPTEN).
# Using it directly means `cmake --preset wasm` works without wrapping the
# invocation in `emcmake`.
#
# Requires EMSDK in the environment, which emsdk_env.{sh,bat} sets.

if(DEFINED ENV{EMSDK})
	set(_uqm_emsdk "$ENV{EMSDK}")
elseif(EXISTS "C:/dev/emsdk/upstream/emscripten")
	# Convenience for the machine this was developed on.
	set(_uqm_emsdk "C:/dev/emsdk")
endif()

if(NOT _uqm_emsdk)
	message(FATAL_ERROR
		"The wasm preset needs Emscripten. Install emsdk and run its "
		"emsdk_env script (which sets EMSDK) before configuring:\n"
		"  git clone https://github.com/emscripten-core/emsdk\n"
		"  cd emsdk && ./emsdk install latest && ./emsdk activate latest\n"
		"  source ./emsdk_env.sh   # or emsdk_env.bat on Windows")
endif()

set(_uqm_em_toolchain
	"${_uqm_emsdk}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")
if(NOT EXISTS "${_uqm_em_toolchain}")
	message(FATAL_ERROR
		"Found EMSDK at ${_uqm_emsdk} but no Emscripten.cmake at "
		"${_uqm_em_toolchain}. Has `emsdk install` been run?")
endif()

include("${_uqm_em_toolchain}")

# Shared memory has to be agreed on by every object in the link, including
# the one FetchContent-built dependency left, SDL3 -- wasm-ld rejects the whole
# link if even one was compiled without it. Setting it here rather than on our
# own targets is what makes that global.
#
# And global is exactly why it has to be conditional. The C game needs threads
# (Starcon2Main) and so needs this; the rewrite does not, and if the flag were
# unconditional the rewrite's build would inherit shared memory anyway -- which
# means SharedArrayBuffer, which means COOP/COEP headers on whoever hosts the
# page. Configuring with -DUQM_LEGACY=OFF is what turns it off.
#
# For the C game this goes away with the threads: see docs/unthread.md §7.6.
if(NOT DEFINED UQM_LEGACY OR UQM_LEGACY)
	set(CMAKE_C_FLAGS_INIT "-pthread")
	set(CMAKE_CXX_FLAGS_INIT "-pthread")
endif()
