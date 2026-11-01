# agave web browser
# 
# Copyright (C) 2024 Zhang Ji Peng
# Contact: onecoolx@gmail.com

file(GLOB_RECURSE SOURCES ${PROJ_ROOT}/dist/src/*.c)

include_directories(${PROJ_ROOT}/dist ${PROJ_ROOT}/dist/include)

set(LIB_DEPS dist)

add_library(${LIB_DEPS} STATIC ${SOURCES})

# MSVC searches every directory on the include stack; Clang/GCC only the directory of
# the including file. The vendored libraries keep their private headers next to their
# sources (dist/src/jpeg/jpegint.h etc.), so add those directories to *this target
# only* - exposing them globally would make e.g. dist/src/curl/FormData.h shadow
# WebCore's platform/network/FormData.h.
target_include_directories(${LIB_DEPS} PRIVATE
    ${PROJ_ROOT}/dist/src/jpeg
    ${PROJ_ROOT}/dist/src/png
    ${PROJ_ROOT}/dist/src/xml2
    ${PROJ_ROOT}/dist/src/xslt
    ${PROJ_ROOT}/dist/src/exslt
    ${PROJ_ROOT}/dist/src/curl
    ${PROJ_ROOT}/dist/src/polarssl
)

if (MSVC)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /wd4267 /wd4244 /wd4311 /wd4312 /wd4996")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /wd4267 /wd4244 /wd4311 /wd4312 /wd4996")
endif()

if (NOT MSVC)
    # We link the vendored libxml2 statically; without this it emits its own
    # DllMain(), which collides with the one inside MinGW's libmingwex.a.
    target_compile_definitions(${LIB_DEPS} PRIVATE LIBXML_STATIC)
endif()

install(TARGETS ${LIB_DEPS} LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)

