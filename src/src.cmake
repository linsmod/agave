# agave web browser
# 
# Copyright (C) 2024 Zhang Ji Peng
# Contact: onecoolx@gmail.com

# CONFIGURE_DEPENDS: tools/build_project.ps1 only runs cmake when the cache is
# missing, so without this a newly added .cpp under these directories is
# silently left out of the build (undefined symbols at link time).
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
                          ${PROJ_ROOT}/src/webcore/css/*.cpp
                          ${PROJ_ROOT}/src/webcore/dom/*.cpp
                          ${PROJ_ROOT}/src/webcore/editing/*.cpp
                          ${PROJ_ROOT}/src/webcore/history/*.cpp
                          ${PROJ_ROOT}/src/webcore/html/*.cpp
                          ${PROJ_ROOT}/src/webcore/ksvg2/*.cpp
                          ${PROJ_ROOT}/src/webcore/loader/*.cpp
                          ${PROJ_ROOT}/src/webcore/page/*.cpp
                          ${PROJ_ROOT}/src/webcore/platform/*.cpp
                          ${PROJ_ROOT}/src/webcore/platform/media/*.cpp
                          ${PROJ_ROOT}/src/webcore/rendering/*.cpp
                          ${PROJ_ROOT}/src/webcore/xml/*.cpp
                          ${PROJ_ROOT}/src/webview/*.cpp
)

list(REMOVE_ITEM SOURCES 
    ${PROJ_ROOT}/src/webcore/platform/unicode/UnicodeIDNA_data.cpp
    ${PROJ_ROOT}/src/webcore/platform/unicode/char_break_data.cpp
    ${PROJ_ROOT}/src/webcore/platform/unicode/line_break_data.cpp
    ${PROJ_ROOT}/src/webcore/platform/unicode/ubidi_props_data.cpp
    ${PROJ_ROOT}/src/webcore/platform/unicode/ucase_props_data.cpp
    ${PROJ_ROOT}/src/webcore/platform/unicode/uchar_props_data.cpp
    ${PROJ_ROOT}/src/webcore/platform/unicode/unorm_props_data.cpp
    ${PROJ_ROOT}/src/webcore/platform/unicode/word_break_data.cpp
)

if (OPT_USE_KJS)
file(GLOB_RECURSE KJS_SOURCES ${PROJ_ROOT}/src/javascript/*.cpp
                              ${PROJ_ROOT}/src/javascript/*.c
                              ${PROJ_ROOT}/src/webcore/buildKJS/*.cpp
                              ${PROJ_ROOT}/src/webcore/buildKJS/*.c
                              ${PROJ_ROOT}/src/webcore/bindings/js/*.cpp
                              ${PROJ_ROOT}/src/wtf/*.cpp
)

set(SOURCES ${SOURCES} ${KJS_SOURCES})

list(REMOVE_ITEM SOURCES 
    ${PROJ_ROOT}/src/javascript/pcre/chartables.c
    ${PROJ_ROOT}/src/javascript/pcre/ucptable.c
    ${PROJ_ROOT}/src/javascript/pcre/dftables.c
    ${PROJ_ROOT}/src/webcore/buildKJS/CSSPropertyNames.c
    ${PROJ_ROOT}/src/webcore/buildKJS/CSSValueKeywords.c
    ${PROJ_ROOT}/src/webcore/buildKJS/DocTypeStrings.cpp
    ${PROJ_ROOT}/src/webcore/buildKJS/tokenizer.cpp
    ${PROJ_ROOT}/src/webcore/buildKJS/JSHTMLInputElementBaseTable.cpp
)

elseif (OPT_USE_QJS)
file(GLOB_RECURSE QJS_SOURCES ${PROJ_ROOT}/src/webcore/buildQJS/*.cpp
                              ${PROJ_ROOT}/src/webcore/buildQJS/*.c
                              ${PROJ_ROOT}/src/webcore/bindings/qjs/*.cpp
                              ${PROJ_ROOT}/src/wtf/*.cpp
                              ${PROJ_ROOT}/src/core/*.cpp
)

set(SOURCES ${SOURCES} ${QJS_SOURCES})

list(REMOVE_ITEM SOURCES 
    ${PROJ_ROOT}/src/webcore/buildQJS/CSSPropertyNames.c
    ${PROJ_ROOT}/src/webcore/buildQJS/CSSValueKeywords.c
    ${PROJ_ROOT}/src/webcore/buildQJS/DocTypeStrings.cpp
    ${PROJ_ROOT}/src/webcore/buildQJS/tokenizer.cpp
    ${PROJ_ROOT}/src/webcore/page/InspectorController.cpp
    ${PROJ_ROOT}/src/wtf/FastMallocPCRE.cpp
)
endif()

set(LIB_NAME agave)

# AntRuntime.cpp is only built when the Ant runtime is enabled.
if (NOT OPT_JS_ANT)
    list(REMOVE_ITEM SOURCES ${PROJ_ROOT}/src/javascript/ant_backend/AntRuntime.cpp)
endif()
# Select exactly one network backend implementation.
if (OPT_NET_BACKEND STREQUAL "native")
    list(REMOVE_ITEM SOURCES
        ${PROJ_ROOT}/src/webcore/platform/network/curl/ResourceHandleCurl.cpp
        ${PROJ_ROOT}/src/webcore/platform/network/curl/ResourceHandleManagerCurl.cpp)
else()
    list(REMOVE_ITEM SOURCES
        ${PROJ_ROOT}/src/webcore/platform/network/native/ResourceHandleNet.cpp
        ${PROJ_ROOT}/src/webcore/platform/network/native/ResourceHandleManagerNet.cpp
        ${PROJ_ROOT}/src/webcore/platform/network/native/NetworkTypes.cpp
        ${PROJ_ROOT}/src/webcore/platform/network/native/win32/WinHttpSession.cpp)
endif()

add_library(${LIB_NAME} ${SOURCES})

# Ant engine runtime (task 1-2): the JS engine and its bridge live in
# antjsb.dll, built from 3rdparty/ant. Enabled with -DOPT_JS_ANT=ON.
if (OPT_JS_ANT)
    target_sources(${LIB_NAME} PRIVATE
        ${PROJ_ROOT}/src/javascript/ant_backend/AntRuntime.cpp
        ${PROJ_ROOT}/src/javascript/ant_backend/RealmDocument.cpp
        ${PROJ_ROOT}/src/javascript/ant_backend/AntBindings.cpp)
    # Generated interface tables (tools/gen_bindings.ps1 -Generator ANT): one
    # buildANT/Ant<Interface>.cpp per migrated IDL, signed in like buildKJS/ and
    # buildQJS/. CONFIGURE_DEPENDS so a newly generated file is picked up by the
    # next build without a manual re-run of cmake.
    file(GLOB ANT_BINDING_SOURCES CONFIGURE_DEPENDS
        ${PROJ_ROOT}/src/webcore/buildANT/*.cpp)
    if (ANT_BINDING_SOURCES)
        target_sources(${LIB_NAME} PRIVATE ${ANT_BINDING_SOURCES})
    endif()
    target_include_directories(${LIB_NAME} PRIVATE
        ${PROJ_ROOT}/src/javascript/ant_backend
        ${PROJ_ROOT}/3rdparty/ant/include)
    target_compile_definitions(${LIB_NAME} PRIVATE AGB_USE_DLL)
    target_link_libraries(${LIB_NAME} PRIVATE ${PROJ_ROOT}/3rdparty/ant/lib/antjsb.dll.a)
    add_custom_command(TARGET ${LIB_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${PROJ_ROOT}/3rdparty/ant/bin/antjsb.dll
            $<TARGET_FILE_DIR:${LIB_NAME}>/antjsb.dll)
    # antjsb is built -O2 -g (release + debug info); keep its PDB next to the
    # DLL so cdb resolves engine/bridge frames without extra setup.
    if (EXISTS ${PROJ_ROOT}/3rdparty/ant/bin/antjsb.pdb)
        add_custom_command(TARGET ${LIB_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${PROJ_ROOT}/3rdparty/ant/bin/antjsb.pdb
                $<TARGET_FILE_DIR:${LIB_NAME}>/antjsb.pdb)
    endif()
endif()

# Native network backend (task 3-1): system stack (WinHTTP/SChannel) instead of
# the bundled curl + PolarSSL. -DOPT_NET_BACKEND=native
if (OPT_NET_BACKEND STREQUAL "native")

    target_sources(${LIB_NAME} PRIVATE
        ${PROJ_ROOT}/src/webcore/platform/network/native/NetworkTypes.cpp
        ${PROJ_ROOT}/src/webcore/platform/network/native/win32/WinHttpSession.cpp
        ${PROJ_ROOT}/src/webcore/platform/network/native/ResourceHandleManagerNet.cpp
        ${PROJ_ROOT}/src/webcore/platform/network/native/ResourceHandleNet.cpp)
    target_include_directories(${LIB_NAME} PRIVATE
        ${PROJ_ROOT}/src/webcore/platform/network/native)
    target_compile_definitions(${LIB_NAME} PRIVATE OPT_NET_BACKEND_NATIVE)
    target_link_libraries(${LIB_NAME} PRIVATE winhttp)
endif()

# Real media playback: waveOut for audio output (MediaPipeline.cpp). winmm is
# part of Windows, so this is unconditional on WIN32.
if (WIN32)
    target_link_libraries(${LIB_NAME} PRIVATE winmm)
endif()

set(HEADERS ${PROJ_ROOT}/include
            ${PROJ_ROOT}/src
            ${PROJ_ROOT}/src/webcore
            ${PROJ_ROOT}/src/webcore/css
            ${PROJ_ROOT}/src/webcore/dom
            ${PROJ_ROOT}/src/webcore/bridge
            ${PROJ_ROOT}/src/webcore/editing
            ${PROJ_ROOT}/src/webcore/html
            ${PROJ_ROOT}/src/webcore/history
            ${PROJ_ROOT}/src/webcore/loader
            ${PROJ_ROOT}/src/webcore/page
            ${PROJ_ROOT}/src/webcore/rendering
            ${PROJ_ROOT}/src/webcore/platform
            ${PROJ_ROOT}/src/webcore/platform/graphics
            ${PROJ_ROOT}/src/webcore/platform/picasso
            ${PROJ_ROOT}/src/webcore/platform/media
            ${PROJ_ROOT}/src/webcore/platform/unicode
            ${PROJ_ROOT}/src/webcore/platform/network
            ${PROJ_ROOT}/src/webcore/platform/network/curl
            ${PROJ_ROOT}/src/webcore/platform/image-decoders
            ${PROJ_ROOT}/src/webcore/platform/image-decoders/jpeg
            ${PROJ_ROOT}/src/webcore/platform/image-decoders/gif
            ${PROJ_ROOT}/src/webcore/platform/image-decoders/png
            ${PROJ_ROOT}/src/webcore/platform/image-decoders/bmp
            ${PROJ_ROOT}/src/webcore/platform/image-decoders/ico
            ${PROJ_ROOT}/src/webcore/xml
            ${PROJ_ROOT}/src/webcore/ksvg2
            ${PROJ_ROOT}/src/webcore/ksvg2/svg
            ${PROJ_ROOT}/src/webcore/ksvg2/css
            ${PROJ_ROOT}/src/webcore/ksvg2/misc
            ${PROJ_ROOT}/src/webcore/ksvg2/events
            ${PROJ_ROOT}/src/webview
            ${PROJ_OUT}/include
            ${PROJ_OUT}/
)

if (OPT_USE_KJS)
set(KJS_HEADERS ${PROJ_ROOT}/src/wtf
                ${PROJ_ROOT}/src/javascript
                ${PROJ_ROOT}/src/javascript/pcre
                ${PROJ_ROOT}/src/javascript/kjs
                ${PROJ_ROOT}/src/javascript/bindings
                ${PROJ_ROOT}/src/javascript/bindings/c
                ${PROJ_ROOT}/src/webcore/buildKJS
                ${PROJ_ROOT}/src/webcore/bindings/js
)

set(HEADERS ${HEADERS} ${KJS_HEADERS})
elseif (OPT_USE_QJS)
set(QJS_HEADERS ${PROJ_ROOT}/src/wtf
                ${PROJ_ROOT}/src/webcore/buildQJS
                ${PROJ_ROOT}/src/webcore/bindings/qjs
                ${PROJ_ROOT}/src/core
)

set(HEADERS ${HEADERS} ${QJS_HEADERS})
endif()

target_include_directories(${LIB_NAME} PRIVATE ${HEADERS})

if (MSVC)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /wd4838 /wd4305 /wd4291 /wd4065 /wd4146 /wd4756")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /wd4838 /wd4305 /wd4291 /wd4065 /wd4146 /wd4756")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-misleading-indentation -Wno-narrowing")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-misleading-indentation -Wno-narrowing")
endif()

target_link_libraries(${LIB_NAME} PRIVATE ${LIB_DEPS})

target_compile_definitions(${LIB_NAME} PRIVATE CURL_STATICLIB)

# _EXPORT_ selects __declspec(dllexport) in include/macross.h; it must be defined
# whenever *building* the library (not only for MSVC).
target_compile_definitions(${LIB_NAME} PRIVATE _EXPORT_)

# picasso is the graphics backend on every platform, not just MSVC
target_link_libraries(${LIB_NAME} PRIVATE picasso2_sw)

set_target_properties(${LIB_NAME} PROPERTIES VERSION ${VERSION_INFO} SOVERSION 1)

add_dependencies(${LIB_NAME} ${LIB_DEPS} ${PICASSO_NAME})

if (OPT_USE_QJS)
    add_dependencies(${LIB_NAME} ${QJS_NAME})
    target_link_libraries(${LIB_NAME} PRIVATE qjslib)
endif()

install(TARGETS ${LIB_NAME} LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)


