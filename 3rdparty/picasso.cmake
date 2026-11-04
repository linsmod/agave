# agave web browser
#
# Copyright (C) 2024 Zhang Ji Peng
# Contact: onecoolx@gmail.com

# picasso 2.8.0 is vendored under 3rdparty/picasso and built as a normal
# subdirectory of this project.
#
# It used to be an ExternalProject: the tarball packages/picasso-2.8.0.tar.gz
# was unpacked into the build tree and patched with copies of a few files from
# packages/patchs/. Every real fix therefore lived outside the repository, and
# a fresh build (or a wiped build directory) silently restored the original
# sources - the graphics backend could not be debugged. The tarball is kept in
# packages/ for reference only; nothing builds from it any more.

set(PICASSO_SOURCE "${PROJ_ROOT}/3rdparty/picasso")

if (NOT EXISTS "${PICASSO_SOURCE}/CMakeLists.txt")
  message(FATAL_ERROR "picasso sources are missing from ${PICASSO_SOURCE}")
endif()

# The switch set the ExternalProject used to pass on the command line. They are
# cache variables so the vendored CMakeLists' defaults do not re-enable the
# parts we do not build (extensions, demos, tests) or formats we do not use.
#
# BUILD_SHARED_LIBS is special: the main project sets the same variable to ON
# to build libagave as a DLL, so picasso is switched to static with a plain
# variable for the duration of add_subdirectory and the value is put back right
# after. Forcing it as a cache entry turned libagave into a static archive and
# broke libagave_webview's imports.
set(_picasso_saved_build_shared "${BUILD_SHARED_LIBS}")

set(BUILD_SHARED_LIBS   OFF)
set(OPT_EXTENSIONS      OFF CACHE BOOL "" FORCE)
set(OPT_DEMOS           OFF CACHE BOOL "" FORCE)
set(OPT_TESTS           OFF CACHE BOOL "" FORCE)
set(OPT_UNITTEST        OFF CACHE BOOL "" FORCE)
set(OPT_FAST_COPY       OFF CACHE BOOL "" FORCE)
set(OPT_FORMAT_ABGR     OFF CACHE BOOL "" FORCE)
set(OPT_FORMAT_ARGB     OFF CACHE BOOL "" FORCE)
set(OPT_FORMAT_RGB555   OFF CACHE BOOL "" FORCE)

add_subdirectory("${PICASSO_SOURCE}" "${PROJ_OUT}/picasso")

set(BUILD_SHARED_LIBS "${_picasso_saved_build_shared}")

# The ExternalProject passed these two flags on the command line because the
# sanitizer instrumentation it inherited from the main build does not mix well
# with picasso's aligned SIMD paths. Keep the same effect for the target.
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(picasso2_sw PRIVATE -fno-sanitize=alignment -fno-sanitize=undefined)
endif()

# Everything in this project includes <picasso/picasso.h> and gets it from
# ${PROJ_OUT}/include, which is what the ExternalProject's install step used to
# populate. Keep the same layout without an install step.
file(GLOB PICASSO_PUBLIC_HEADERS "${PICASSO_SOURCE}/include/*.h")
file(COPY ${PICASSO_PUBLIC_HEADERS} DESTINATION "${PROJ_OUT}/include/picasso")

include_directories(${PROJ_OUT}/include)
link_directories(${PROJ_OUT}/lib)
