#!/usr/bin/env python3
# Patches Butterscotch source files to add iOS support.
# Safe to run multiple times (checks if already patched).
#
# Patched files:
#   CMakeLists.txt  — adds ios platform block
#   src/gl/gl_renderer.h — adds PLATFORM_IOS to the GLES guard

import sys, re, os

if len(sys.argv) != 2:
    print("Usage: patch_cmake.py <path/to/CMakeLists.txt>")
    sys.exit(1)

path = sys.argv[1]
with open(path, "r") as f:
    src = f.read()

if "PLATFORM_IOS" in src:
    print("Already patched.")
    sys.exit(0)

# 1. Include .m files in PLATFORM_SOURCES (for VirtualController.m)
src = src.replace(
    "file(GLOB PLATFORM_SOURCES src/${PLATFORM}/*.c)",
    "file(GLOB PLATFORM_SOURCES src/${PLATFORM}/*.c src/${PLATFORM}/*.m)"
)

# 2. Set MACOSX_BUNDLE on the iOS target after add_executable
src = src.replace(
    "if(PLATFORM STREQUAL \"android\")\n    add_library(butterscotch SHARED ${SOURCES} ${PLATFORM_SOURCES} ${AUDIO_SOURCES})\nelse()\n    add_executable(butterscotch ${SOURCES} ${PLATFORM_SOURCES} ${AUDIO_SOURCES})\nendif()",
    """if(PLATFORM STREQUAL "android")
    add_library(butterscotch SHARED ${SOURCES} ${PLATFORM_SOURCES} ${AUDIO_SOURCES})
else()
    add_executable(butterscotch ${SOURCES} ${PLATFORM_SOURCES} ${AUDIO_SOURCES})
endif()
if(PLATFORM STREQUAL "ios")
    set_target_properties(butterscotch PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.butterscotch.Undertale"
        MACOSX_BUNDLE_BUNDLE_NAME "Undertale"
    )
endif()"""
)

# 3. Insert iOS platform block before the fatal "Unknown platform" error
IOS_BLOCK = '''elseif(PLATFORM STREQUAL "ios")
    if(NOT ENABLE_MODERN_GL)
        message(FATAL_ERROR "iOS requires modern GL!")
    endif()
    file(GLOB GL_SOURCES src/gl/*.c src/gl_common/*.c src/image/*.c)
    target_sources(butterscotch PRIVATE ${GL_SOURCES})
    target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src/gl)
    target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src/gl_common)
    target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src/image)
    add_compile_definitions(ENABLE_GLES PLATFORM_IOS USE_SDL2)
    # VirtualController thumbstick vs d-pad
    option(VIRTUAL_CONTROLLER_THUMBSTICK "Use thumbstick instead of d-pad" OFF)
    if(VIRTUAL_CONTROLLER_THUMBSTICK)
        add_compile_definitions(VIRTUAL_CONTROLLER_THUMBSTICK)
    endif()
    # SDL2 — passed in via -DSDL2_FRAMEWORK_PATH (no SDL2Config.cmake in iOS DMG)
    target_include_directories(butterscotch PRIVATE "${SDL2_FRAMEWORK_PATH}/Headers")
    # iOS frameworks
    target_link_libraries(butterscotch PRIVATE
        "${SDL2_FRAMEWORK_PATH}/SDL2"
        "-framework UIKit"
        "-framework GameController"
        "-framework CoreMotion"
        "-framework CoreGraphics"
        "-framework OpenGLES"
        "-framework AVFoundation"
        "-framework AudioToolbox"
        bzip2 stb_ds sha1 stb_vorbis
    )
    target_include_directories(butterscotch PUBLIC vendor/stb/image)
    target_include_directories(butterscotch PUBLIC vendor/stb/vorbis)
'''

src = src.replace(
    'else()\n    message(FATAL_ERROR "Unknown platform! ${PLATFORM}")',
    IOS_BLOCK + 'else()\n    message(FATAL_ERROR "Unknown platform! ${PLATFORM}")'
)

with open(path, "w") as f:
    f.write(src)

print("CMakeLists.txt patched for iOS.")

# ── Helper: patch a file with a single string replacement ──────────────────
def patch_file(filepath, old, new, already_tag, description):
    if not os.path.exists(filepath):
        print(f"Warning: {filepath} not found — skipping {description}.")
        return
    with open(filepath, "r") as f:
        src = f.read()
    if already_tag in src:
        print(f"{os.path.basename(filepath)} already patched ({description}).")
        return
    if old not in src:
        print(f"Warning: expected pattern not found in {filepath} — skipping {description}.")
        return
    src = src.replace(old, new)
    with open(filepath, "w") as f:
        f.write(src)
    print(f"{os.path.basename(filepath)} patched for iOS ({description}).")

src_dir = os.path.join(os.path.dirname(path), "src")

# ── Patch src/gl/gl_renderer.h ──────────────────────────────────────────────
# Already guards glad for Emscripten/Android; extend the guard to iOS too.
patch_file(
    os.path.join(src_dir, "gl", "gl_renderer.h"),
    '#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)\n#include <GLES3/gl3.h>\n#else\n#include <glad/glad.h>\n#endif',
    '#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(PLATFORM_IOS)\n#include <OpenGLES/ES3/gl.h>\n#include <OpenGLES/ES3/glext.h>\n#else\n#include <glad/glad.h>\n#endif',
    'PLATFORM_IOS',
    "OpenGLES guard in header"
)

# ── Patch src/gl/gl_renderer.c ──────────────────────────────────────────────
# The .c file has its own bare #include <glad/glad.h> at the top (line 8).
# Replace it with the same guarded include used in the header.
patch_file(
    os.path.join(src_dir, "gl", "gl_renderer.c"),
    '#include <glad/glad.h>',
    '#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(PLATFORM_IOS)\n#include <OpenGLES/ES3/gl.h>\n#include <OpenGLES/ES3/glext.h>\n#else\n#include <glad/glad.h>\n#endif',
    'PLATFORM_IOS',
    "OpenGLES guard in .c"
)

# ── Patch src/gl_common/gl_common.c (and .h if present) ─────────────────────
# gl_common sources may also include glad directly.
for fname in ("gl_common.c", "gl_common.h"):
    fpath = os.path.join(src_dir, "gl_common", fname)
    if os.path.exists(fpath):
        with open(fpath) as f:
            content = f.read()
        if '#include <glad/glad.h>' in content and 'PLATFORM_IOS' not in content:
            content = content.replace(
                '#include <glad/glad.h>',
                '#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(PLATFORM_IOS)\n#include <OpenGLES/ES3/gl.h>\n#include <OpenGLES/ES3/glext.h>\n#else\n#include <glad/glad.h>\n#endif'
            )
            with open(fpath, "w") as f:
                f.write(content)
            print(f"{fname} patched for iOS (OpenGLES guard).")

# ── Patch src/audio/miniaudio/ma_audio_system.c ─────────────────────────────
# miniaudio.h includes AVFoundation which uses ObjC @class syntax.
# That's fine in an .m file but breaks plain C compilation.
# Rename the file to .m so clang compiles it as Objective-C.
ma_c  = os.path.join(src_dir, "audio", "miniaudio", "ma_audio_system.c")
ma_m  = os.path.join(src_dir, "audio", "miniaudio", "ma_audio_system.m")
if os.path.exists(ma_c) and not os.path.exists(ma_m):
    import shutil
    shutil.copy2(ma_c, ma_m)
    print("ma_audio_system.c copied to .m for iOS ObjC compilation.")
# Tell CMake to use the .m file instead of the .c file on iOS.
# We do this by adding a PLATFORM_SOURCES glob for .m in the audio dir,
# which we handle by patching the IOS_BLOCK in CMakeLists.txt to also
# remove the .c audio source and add the .m version.
# Simpler: just add a compile flag to treat the file as ObjC via CMake
# set_source_files_properties. We do it by appending to the ios block below
# if not already done.
# Actually the cleanest approach: add the -x objective-c flag for this file
# in the iOS CMake block. But we already wrote CMakeLists.txt — patch it now.
cmake_path = path  # CMakeLists.txt
with open(cmake_path, "r") as f:
    cmake_src = f.read()
OLD_AUDIO_LINE = '    target_sources(butterscotch PRIVATE ${GL_SOURCES})'
NEW_AUDIO_LINE = '''    target_sources(butterscotch PRIVATE ${GL_SOURCES})
    # miniaudio.h pulls in AVFoundation which needs ObjC — compile as .m
    set_source_files_properties(
        ${CMAKE_SOURCE_DIR}/src/audio/miniaudio/ma_audio_system.c
        PROPERTIES COMPILE_FLAGS "-x objective-c"
    )'''
if 'COMPILE_FLAGS "-x objective-c"' not in cmake_src and OLD_AUDIO_LINE in cmake_src:
    cmake_src = cmake_src.replace(OLD_AUDIO_LINE, NEW_AUDIO_LINE)
    with open(cmake_path, "w") as f:
        f.write(cmake_src)
    print("CMakeLists.txt patched: ma_audio_system.c flagged as ObjC.")
