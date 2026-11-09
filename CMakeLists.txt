cmake_minimum_required(VERSION 3.16)

project(Agave LANGUAGES C CXX ASM HOMEPAGE_URL https://github.com/onecoolx/agave)

# The MSVC build relied on "#pragma comment(lib, ...)" inside the sources; with
# GCC/Clang on Windows the system libraries have to be listed explicitly.
if (WIN32 AND NOT MSVC)
    link_libraries(ws2_32 iphlpapi crypt32 advapi32 user32 gdi32 shell32 ole32 oleaut32 uuid)
    # zig's compiler-rt (linked last by the driver) expects the MinGW libmingwex
    # helpers (fp constants, copysignl, ...). Linking it fully collides with a few
    # symbols the vendored code also defines (gettimeofday), so tell lld to take
    # the first definition instead of erroring.
    link_libraries("C:/msys64/mingw64/lib/libmingwex.a")

endif()

set (CMAKE_CXX_STANDARD 98)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(BUILD_SHARED_LIBS ON) # build shared library, OFF for static

set(PROJ_ROOT ${CMAKE_CURRENT_LIST_DIR})
set(PROJ_OUT ${CMAKE_CURRENT_BINARY_DIR})

set(VERSION_MAJOR 1)
set(VERSION_MINOR 0)
set(VERSION_MICRO 0)
set(VERSION_INFO ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_MICRO})

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

if(POLICY CMP0114)
  cmake_policy(SET CMP0114 NEW)
endif()

if (NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build Type" FORCE)
endif()

option(OPT_USE_INTERNAL_LIBS "Build internal libs" ON)

include (${CMAKE_CURRENT_LIST_DIR}/build/configs.cmake)
include (${CMAKE_CURRENT_LIST_DIR}/build/defines.cmake)
include (${CMAKE_CURRENT_LIST_DIR}/3rdparty/3rdparty.cmake)
if (OPT_USE_INTERNAL_LIBS)
include (${CMAKE_CURRENT_LIST_DIR}/dist/allinone.cmake)
endif()

OPTION_MACRO_DEFINE(OPT_PNG_SUPPORT PNG_SUPPORT "Build support PNG image format." ON)
OPTION_MACRO_DEFINE(OPT_JPEG_SUPPORT JPEG_SUPPORT "Build support JPEG image format." ON)
OPTION_MACRO_DEFINE(OPT_GIF_SUPPORT GIF_SUPPORT "Build support GIF image format." ON)
OPTION_MACRO_DEFINE(OPT_FILESYSTEM ENABLE_FILESYSTEM "Build support file system." ON)
OPTION_MACRO_DEFINE(OPT_FTPDIR ENABLE_FTPDIR "Build support list ftp dir." ON)
OPTION_MACRO_DEFINE(OPT_COOKIES ENABLE_COOKIES "Build cookie support." ON)
OPTION_MACRO_DEFINE(OPT_FILECOOKIE ENABLE_FILECOOKIE "Build file support with cookies." ON)
OPTION_MACRO_DEFINE(OPT_HTTPCACHE ENABLE_HTTPCACHE "Build http disk cache support." ON)
OPTION_MACRO_DEFINE(OPT_SSL ENABLE_SSL "Build with SSL support." ON)
OPTION_MACRO_DEFINE(OPT_AJAX ENABLE_AJAX "Build support ajax." ON)
OPTION_MACRO_DEFINE(OPT_SSLFILE ENABLE_SSLFILE "Build with file support with SSL." ON)
OPTION_MACRO_DEFINE(OPT_XML ENABLE_XML "Build XML support." ON)
OPTION_MACRO_DEFINE(OPT_SVG ENABLE_SVG "Build SVG support (ksvg2)." ON)
OPTION_MACRO_DEFINE(OPT_XSLT ENABLE_XSLT "Build XSLT support." ON)
OPTION_MACRO_DEFINE(OPT_XPATH ENABLE_XPATH "Build XPATH support." ON)
OPTION_MACRO_DEFINE(OPT_FOCUSRING FOCUSRING_DRAWING "Build support focus ring of owner drawing style." OFF)
OPTION_MACRO_DEFINE(OPT_BIG5_SUPPORT ENABLE_BIG5_SUPPORT "Build support charset BIG5." ON)
OPTION_MACRO_DEFINE(OPT_GBK_SUPPORT ENABLE_GBK_SUPPORT "Build support charset GBK." ON)
OPTION_MACRO_DEFINE(OPT_GB2312_SUPPORT ENABLE_GB2312_SUPPORT "Build support charset GB2312." ON)
OPTION_MACRO_DEFINE(OPT_EUCJP_SUPPORT ENABLE_EUCJP_SUPPORT "Build support charset EUCJP." ON)
OPTION_MACRO_DEFINE(OPT_EUCKR_SUPPORT ENABLE_EUCKR_SUPPORT "Build support charset EUCKR." ON)
OPTION_MACRO_DEFINE(OPT_SHIFTJIS_SUPPORT ENABLE_SHIFTJIS_SUPPORT "Build support charset SHIFTJIS." ON)

OPTION_MACRO_DEFINE(OPT_USE_KJS ENABLE_KJS "Build with KJS JavaScript engine." ON)
OPTION_MACRO_DEFINE(OPT_USE_QJS ENABLE_QJS "Build with QuickJS JavaScript engine." OFF)
OPTION_MACRO_DEFINE(OPT_JS_ANT ENABLE_ANTJS "Build with Ant JavaScript engine (antjsb.dll)." OFF)
set(OPT_NET_BACKEND "curl" CACHE STRING "Network backend: curl (bundled) or native (system stack)")

# quickjs is pulled in only when the QuickJS backend is selected (see 3rdparty.cmake)
if (OPT_USE_QJS)
include (${CMAKE_CURRENT_LIST_DIR}/3rdparty/quickjs.cmake)
endif()

include (${CMAKE_CURRENT_LIST_DIR}/src/src.cmake)
# agave_test is a demo host; it is only built together with the examples.
if (OPT_EXAMPLES)
include (${CMAKE_CURRENT_LIST_DIR}/test/test.cmake)
endif()

option(OPT_UNITTEST "Build unit tests" OFF)
if (OPT_UNITTEST)
enable_testing()
include (${CMAKE_CURRENT_LIST_DIR}/unit_tests/unit_tests.cmake)
endif()

option(OPT_EXAMPLES "Build examples" OFF)

# The touchweb example is the phone-style browser UI (examples/touchweb). It is
# built on its own so a desktop-only build does not have to compile ~50 extra
# sources plus sqlite and vgcl.
option(OPT_TOUCHWEB "Build the touchweb mobile browser example" OFF)
if (OPT_EXAMPLES OR OPT_TOUCHWEB)
include (${CMAKE_CURRENT_LIST_DIR}/examples/examples.cmake)
endif()

option(OPT_SHELL "Build the Win32 browser shell" OFF)
if (OPT_SHELL AND WIN32)
    # The Win32 control is part of libagave now: one DLL exports both the
    # MaCross engine API and the agave_webview_* control API.
    target_sources(${LIB_NAME} PRIVATE
        ${PROJ_ROOT}/src/webview_control/AgaveWebView.c
        ${PROJ_ROOT}/src/webview_control/AgwViewBackend.c
        ${PROJ_ROOT}/src/webview_control/AgwViewBackendProcess.c)
    set_source_files_properties(
        ${PROJ_ROOT}/src/webview_control/AgaveWebView.c
        PROPERTIES COMPILE_DEFINITIONS AGW_BUILD_DLL)
    target_link_libraries(${LIB_NAME} PRIVATE user32 gdi32)
    add_library(agave_webview ALIAS ${LIB_NAME})

    add_executable(agave_shell WIN32 ${PROJ_ROOT}/examples/browser_shell/main.c)
    target_include_directories(agave_shell PRIVATE ${PROJ_ROOT}/include ${PROJ_OUT})
    target_compile_definitions(agave_shell PRIVATE AGW_USE_DLL)
    # dist carries the vendored libpng/zlib: the probe writes --shot .png files
    # itself (libagave links dist privately, which does not reach an exe).
    target_link_libraries(agave_shell PRIVATE ${LIB_NAME} dist user32 gdi32 shell32 comdlg32)

    # Engine side of the B2 process backend: one console process per view, no
    # window; the pixels land in the control's shared section (AgwViewIpc.h).
    add_executable(agave_render_host ${PROJ_ROOT}/examples/render_host/main.c)
    target_include_directories(agave_render_host PRIVATE ${PROJ_ROOT}/include ${PROJ_OUT})
    target_compile_definitions(agave_render_host PRIVATE AGW_USE_DLL)
    target_link_libraries(agave_render_host PRIVATE ${LIB_NAME} user32 gdi32)

    add_executable(agave_embed WIN32 ${PROJ_ROOT}/examples/embed_demo/main.c)
    target_include_directories(agave_embed PRIVATE ${PROJ_ROOT}/include ${PROJ_OUT})
    target_compile_definitions(agave_embed PRIVATE AGW_USE_DLL)
    target_link_libraries(agave_embed PRIVATE ${LIB_NAME} user32 gdi32)

    # Headless test host: renders into a memory buffer, prints title/progress/
    # timings and saves a BMP. This is what the perf and probe scripts run, and
    # it is the only way a page can report results back (via document.title).
    add_executable(agave_view_host ${PROJ_ROOT}/examples/view_host/main.cpp)
    target_include_directories(agave_view_host PRIVATE ${PROJ_ROOT}/include ${PROJ_OUT})
    target_link_libraries(agave_view_host PRIVATE ${LIB_NAME} user32 gdi32)
endif()


include(CheckIncludeFile)
check_include_file(stdint.h HAVE_STDINT_H)
check_include_file(stddef HAVE_STDDEF_H)
check_include_file(stdlib.h HAVE_STDLIB_H)
check_include_file(errno.h HAVE_ERRNO_H)
check_include_file(float.h HAVE_FLOAT_H)
check_include_file(fcntl.h HAVE_FCNTL_H)
check_include_file(string.h HAVE_STRING_H)
check_include_file(inttypes.h HAVE_INTTYPES_H)
check_include_file(limits.h HAVE_LIMITS_H)
check_include_file(memory.h HAVE_MEMORY_H)
check_include_file(unistd.h HAVE_UNISTD_H)
check_include_file(sys/time.h HAVE_SYS_TIME_H)
check_include_file(sys/types.h HAVE_SYS_TYPES_H)
check_include_file(sys/stat.h HAVE_SYS_STAT_H)

include(CheckFunctionExists)
check_function_exists(strcasecmp HAVE_STRCASECMP)
check_function_exists(strncasecmp HAVE_STRNCASECMP)
check_function_exists(ftime HAVE_FTIME)
check_function_exists(gettimeofday HAVE_GETTIMEOFDAY)
check_function_exists(mmap HAVE_MMAP)
check_function_exists(munmap HAVE_MUNMAP)

configure_file(
    "${PROJ_ROOT}/build/mconfig.h.in"
    "${PROJ_OUT}/mconfig.h"
)

# <wtf/Platform.h> is force-included for GCC/Clang builds and pulls in mconfig.h,
# so every target needs the generated header directory on its include path.
include_directories(${PROJ_OUT})

# polyfills.js is read at runtime from the directory of the running executable
# (loadPolyfills() in src/javascript/ant_backend/AntRuntime.cpp) instead of being
# compiled in, so it can be edited without relinking. Keep a fresh copy next to
# the binaries, otherwise every run prints
#   [agave] polyfills.js not found (set AGB_POLYFILLS to load one)
set(AGB_POLYFILLS ${PROJ_ROOT}/src/javascript/ant_backend/polyfills.js)
if (EXISTS ${AGB_POLYFILLS})
    add_custom_command(
        OUTPUT ${PROJ_OUT}/polyfills.js
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${AGB_POLYFILLS} ${PROJ_OUT}/polyfills.js
        DEPENDS ${AGB_POLYFILLS}
        COMMENT "Copying polyfills.js next to the binaries"
        VERBATIM)
    add_custom_target(polyfills ALL DEPENDS ${PROJ_OUT}/polyfills.js)
    foreach (agb_target agave_shell agave_embed agave_view_host touchweb)
        if (TARGET ${agb_target})
            add_dependencies(${agb_target} polyfills)
        endif()
    endforeach()
else()
    message(WARNING "polyfills.js not found at ${AGB_POLYFILLS}; pages will run without the DOM polyfills")
endif()

# prelude.js: the window bootstrap shims (window/document helpers, network and
# storage shims, event plumbing). Same deal as polyfills.js - loaded at runtime
# by loadPrelude(), so it can be edited without relinking. Missing file leaves
# pages without their globals, so it is copied next to every binary.
set(AGB_PRELUDE ${PROJ_ROOT}/src/javascript/ant_backend/prelude.js)
if (EXISTS ${AGB_PRELUDE})
    add_custom_command(
        OUTPUT ${PROJ_OUT}/prelude.js
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${AGB_PRELUDE} ${PROJ_OUT}/prelude.js
        DEPENDS ${AGB_PRELUDE}
        COMMENT "Copying prelude.js next to the binaries"
        VERBATIM)
    add_custom_target(prelude ALL DEPENDS ${PROJ_OUT}/prelude.js)
    foreach (agb_target agave_shell agave_embed agave_view_host touchweb)
        if (TARGET ${agb_target})
            add_dependencies(${agb_target} prelude)
        endif()
    endforeach()
else()
    message(WARNING "prelude.js not found at ${AGB_PRELUDE}; pages will run without the window shims")
endif()

# internal.js / downloads.html: the internal page runtime (agave://downloads).
# internal.js is evaluated only for an agave:// document (AntRuntime::
# bindDocument), downloads.html is served by WebView::loadUrl for
# agave://downloads. Both are runtime assets next to the binaries, like
# prelude.js, so they can be edited without relinking.
set(AGB_INTERNAL_JS ${PROJ_ROOT}/src/javascript/ant_backend/internal.js)
set(AGB_INTERNAL_HTML ${PROJ_ROOT}/src/webview/internal/downloads.html)
if (EXISTS ${AGB_INTERNAL_JS} AND EXISTS ${AGB_INTERNAL_HTML})
    add_custom_command(
        OUTPUT ${PROJ_OUT}/internal.js
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${AGB_INTERNAL_JS} ${PROJ_OUT}/internal.js
        DEPENDS ${AGB_INTERNAL_JS}
        COMMENT "Copying internal.js next to the binaries"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PROJ_OUT}/downloads.html
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${AGB_INTERNAL_HTML} ${PROJ_OUT}/downloads.html
        DEPENDS ${AGB_INTERNAL_HTML}
        COMMENT "Copying downloads.html next to the binaries"
        VERBATIM)
    add_custom_target(internalpages ALL DEPENDS ${PROJ_OUT}/internal.js ${PROJ_OUT}/downloads.html)
    foreach (agb_target agave_shell agave_embed agave_view_host touchweb)
        if (TARGET ${agb_target})
            add_dependencies(${agb_target} internalpages)
        endif()
    endforeach()
else()
    message(WARNING "internal.js / downloads.html not found; agave://downloads will be blank")
endif()

# Injected diagnostics: inspector.js (the hover element inspector) and the
# vendored vConsole bundle are read and evaluated by the shell on demand
# (context menu / "=" menu). Ship them next to the binaries like prelude.js so
# the menu works from the build directory without any install step.
set(AGB_INJECT_ASSETS "")
foreach (agb_asset
         ${PROJ_ROOT}/src/javascript/ant_backend/inspector.js
         ${PROJ_ROOT}/src/javascript/ant_backend/vendor/vconsole.min.js)
    if (EXISTS ${agb_asset})
        get_filename_component(agb_asset_name ${agb_asset} NAME)
        add_custom_command(
            OUTPUT ${PROJ_OUT}/${agb_asset_name}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${agb_asset} ${PROJ_OUT}/${agb_asset_name}
            DEPENDS ${agb_asset}
            COMMENT "Copying ${agb_asset_name} next to the binaries"
            VERBATIM)
        list(APPEND AGB_INJECT_ASSETS ${PROJ_OUT}/${agb_asset_name})
    endif()
endforeach()
if (AGB_INJECT_ASSETS)
    add_custom_target(injectassets ALL DEPENDS ${AGB_INJECT_ASSETS})
    foreach (agb_target agave_shell agave_embed agave_view_host touchweb)
        if (TARGET ${agb_target})
            add_dependencies(${agb_target} injectassets)
        endif()
    endforeach()
endif()
