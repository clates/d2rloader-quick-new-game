# Cross-compile Windows x64 DLLs on Linux with clang-cl + lld-link, using the
# MSVC CRT and Windows SDK unpacked by `xwin splat --output ~/.xwin`.
#
#   cmake -S <src> -B <build> -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain-xwin.cmake -DCMAKE_BUILD_TYPE=Release

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER_TYPE LLD)
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_MT llvm-mt)
set(CMAKE_RC_COMPILER llvm-rc)

if(DEFINED ENV{XWIN_DIR})
	set(XWIN "$ENV{XWIN_DIR}")
else()
	set(XWIN "$ENV{HOME}/.xwin")
endif()

set(_inc
	"${XWIN}/crt/include"
	"${XWIN}/sdk/include/ucrt"
	"${XWIN}/sdk/include/um"
	"${XWIN}/sdk/include/shared"
)
set(_libs
	"${XWIN}/crt/lib/x86_64"
	"${XWIN}/sdk/lib/um/x86_64"
	"${XWIN}/sdk/lib/ucrt/x86_64"
)

# /imsvc marks the SDK as system headers, so its warnings stay quiet.
set(_cflags "--target=x86_64-pc-windows-msvc")
foreach(d IN LISTS _inc)
	string(APPEND _cflags " /imsvc\"${d}\"")
endforeach()
set(CMAKE_C_FLAGS_INIT "${_cflags}")
set(CMAKE_CXX_FLAGS_INIT "${_cflags}")

set(_ldflags "")
foreach(d IN LISTS _libs)
	string(APPEND _ldflags " /libpath:\"${d}\"")
endforeach()
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_ldflags}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_ldflags}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_ldflags}")

# llvm-rc runs its own preprocessor and needs the SDK headers (winres.h, etc.).
set(_rcflags "")
foreach(d IN LISTS _inc)
	string(APPEND _rcflags " -I \"${d}\"")
endforeach()
set(CMAKE_RC_FLAGS_INIT "${_rcflags}")

# xwin splats only the release CRT (no msvcrtd.lib) unless run with
# --include-debug-libs, so always use the release DLL runtime.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

set(CMAKE_FIND_ROOT_PATH "${XWIN}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
