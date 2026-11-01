# HTML5 runtime
# 
# Copyright (C) 2024 Zhang Ji Peng
# Contact: onecoolx@gmail.com

if (WIN32)
    add_compile_definitions(WIN32)
    add_compile_definitions(_WIN32)
    add_compile_definitions(_UNICODE=1)
    add_compile_definitions(UNICODE=1)
    add_compile_definitions(_EXPORT_API_)
    add_compile_definitions(_HAS_EXCEPTIONS=0)
    add_compile_definitions(_USE_MATH_DEFINES)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_compile_definitions(_CRT_SECURE_NO_DEPRECATE)
    add_compile_definitions(_CRT_NONSTDC_NO_WARNINGS)
    add_compile_definitions(_SCL_SECURE_NO_WARNINGS)
    add_compile_definitions(_SILENCE_STDEXT_HASH_DEPRECATION_WARNINGS)
    add_compile_definitions(_WINSOCK_DEPRECATED_NO_WARNINGS)
    # MinGW/Zig winsock2.h already provides struct pollfd + POLL* (WSAPoll);
    # keep the vendored curl (dist/src/curl/select.h) from redefining them.
    if (NOT MSVC)
        add_compile_definitions(HAVE_STRUCT_POLLFD=1)
    endif()
endif()

# The code base was written against MSVC, whose CRT headers pull in <stdint.h>
# transitively. GCC/Clang do not, so force it in for every translation unit.
if (NOT MSVC)
    add_compile_options(-include stdint.h)
    # The vendored libxml2 relies on tentative definitions being merged
    # (xmlStringTextNoenc etc. are defined in both SAX2.c and xmlreader.c);
    # GCC 10+/Clang default to -fno-common, which turns them into duplicate symbols.
    add_compile_options(-fcommon)
    # zig/clang may instrument alignment; the engine has legacy unaligned accesses
    add_compile_options(-fno-sanitize=alignment -fno-sanitize=undefined)
    # WebCore headers use ENABLE()/USE()/HAVE() from <wtf/Platform.h> without pulling it
    # in themselves; the MSVC build reached it through config.h first. Use an absolute
    # path so that every target resolves it without needing -I ${PROJ_ROOT}/src.
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -include \"${PROJ_ROOT}/src/wtf/Platform.h\"")
endif()

macro(OPTION_MACRO_DEFINE _option _macro _desc _initval)
option(${_option} ${_desc} ${_initval})
if (${_option})
    set(${_macro} 1)
else()
    set(${_macro} 0)
endif()
endmacro()
