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

# ── Patch src/gl/gl_renderer.h ──────────────────────────────────────────────
# gl_renderer.h already guards glad for __EMSCRIPTEN__ and __ANDROID__.
# We add PLATFORM_IOS to that guard so it uses <OpenGLES/ES3/gl.h> on iOS too.
gl_renderer_h = os.path.join(os.path.dirname(path), "src", "gl", "gl_renderer.h")
if not os.path.exists(gl_renderer_h):
    print(f"Warning: {gl_renderer_h} not found — skipping GLES guard patch.")
else:
    with open(gl_renderer_h, "r") as f:
        hsrc = f.read()

    OLD_GUARD = '#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)\n#include <GLES3/gl3.h>\n#else\n#include <glad/glad.h>\n#endif'
    NEW_GUARD = '#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(PLATFORM_IOS)\n#include <OpenGLES/ES3/gl.h>\n#include <OpenGLES/ES3/glext.h>\n#else\n#include <glad/glad.h>\n#endif'

    if 'PLATFORM_IOS' in hsrc:
        print("gl_renderer.h already patched.")
    elif OLD_GUARD not in hsrc:
        print(f"Warning: expected GLES guard not found in {gl_renderer_h} — skipping.")
    else:
        hsrc = hsrc.replace(OLD_GUARD, NEW_GUARD)
        with open(gl_renderer_h, "w") as f:
            f.write(hsrc)
        print("gl_renderer.h patched for iOS (OpenGLES guard added).")
