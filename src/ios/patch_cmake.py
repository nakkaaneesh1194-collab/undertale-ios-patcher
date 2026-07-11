#!/usr/bin/env python3
# Patches Butterscotch source files to add iOS support.
# Safe to run multiple times (checks if already patched).
#
# Patched files:
#   CMakeLists.txt         — adds ios platform block
#   src/gl/gl_renderer.h   — adds PLATFORM_IOS to the GLES guard
#   src/gl/gl_renderer.c   — guards glad include + capability checks
#   src/gl_common/gl_common.c / gl_common.h — guards glad include
#   src/gl_common/gl_wrappers.h — aliases OES/EXT names to core names on iOS
#   src/audio/miniaudio/ma_audio_system.c — flagged as ObjC in CMake

import sys, os

if len(sys.argv) != 2:
    print("Usage: patch_cmake.py <path/to/CMakeLists.txt>")
    sys.exit(1)

path = sys.argv[1]
src_dir = os.path.join(os.path.dirname(path), "src")

# ── Helper: patch a file with a single string replacement ──────────────────
def patch_file(filepath, old, new, already_tag, description):
    if not os.path.exists(filepath):
        print(f"Warning: {filepath} not found — skipping {description}.")
        return
    with open(filepath, "r") as f:
        content = f.read()
    if already_tag in content:
        print(f"{os.path.basename(filepath)} already patched ({description}).")
        return
    if old not in content:
        print(f"Warning: expected pattern not found in {filepath} — skipping {description}.")
        return
    content = content.replace(old, new)
    with open(filepath, "w") as f:
        f.write(content)
    print(f"{os.path.basename(filepath)} patched for iOS ({description}).")

# ══════════════════════════════════════════════════════════════════════════════
# 1. CMakeLists.txt — add iOS platform block
# ══════════════════════════════════════════════════════════════════════════════
with open(path, "r") as f:
    src = f.read()

if "PLATFORM_IOS" in src:
    print("CMakeLists.txt already patched — skipping CMake edits.")
else:
    # 1a. Include .m files in PLATFORM_SOURCES (for VirtualController.m)
    src = src.replace(
        "file(GLOB PLATFORM_SOURCES src/${PLATFORM}/*.c)",
        "file(GLOB PLATFORM_SOURCES src/${PLATFORM}/*.c src/${PLATFORM}/*.m)"
    )

    # 1b. Set MACOSX_BUNDLE on the iOS target after add_executable
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

    # 1c. Insert iOS platform block before the fatal "Unknown platform" error
    IOS_BLOCK = '''elseif(PLATFORM STREQUAL "ios")
    if(NOT ENABLE_MODERN_GL)
        message(FATAL_ERROR "iOS requires modern GL!")
    endif()
    file(GLOB GL_SOURCES src/gl/*.c src/gl_common/*.c src/image/*.c)
    target_sources(butterscotch PRIVATE ${GL_SOURCES})
    # miniaudio.h pulls in AVFoundation which needs ObjC — compile as ObjC
    set_source_files_properties(
        ${CMAKE_SOURCE_DIR}/src/audio/miniaudio/ma_audio_system.c
        PROPERTIES COMPILE_FLAGS "-x objective-c"
    )
    target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src/gl)
    target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src/gl_common)
    target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src/image)
    add_compile_definitions(ENABLE_GLES PLATFORM_IOS USE_SDL2 GLES_SILENCE_DEPRECATION)
    # VirtualController thumbstick vs d-pad
    option(VIRTUAL_CONTROLLER_THUMBSTICK "Use thumbstick instead of d-pad" OFF)
    if(VIRTUAL_CONTROLLER_THUMBSTICK)
        add_compile_definitions(VIRTUAL_CONTROLLER_THUMBSTICK)
    endif()
    # SDL2 — passed in via -DSDL2_FRAMEWORK_PATH (no SDL2Config.cmake in iOS DMG)
    target_include_directories(butterscotch PRIVATE "${SDL2_FRAMEWORK_PATH}/Headers")
    # libSDL2main.a provides the real main() that calls UIApplicationMain;
    # SDL_main.h #defines main -> SDL_main so our main.c is the entry point.
    find_library(SDL2MAIN_LIB SDL2main
        PATHS "${SDL2_FRAMEWORK_PATH}"
        NO_DEFAULT_PATH
    )
    # iOS frameworks
    target_link_libraries(butterscotch PRIVATE
        "${SDL2_FRAMEWORK_PATH}/SDL2"
        ${SDL2MAIN_LIB}
        "-framework UIKit"
        "-framework GameController"
        "-framework CoreMotion"
        "-framework CoreGraphics"
        "-framework CoreBluetooth"
        "-framework CoreHaptics"
        "-framework QuartzCore"
        "-framework Metal"
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

# ══════════════════════════════════════════════════════════════════════════════
# 2. Patch the CMake ObjC flag for ma_audio_system.c (backfill if already present
#    but missing the set_source_files_properties line)
# ══════════════════════════════════════════════════════════════════════════════
with open(path, "r") as f:
    cmake_src = f.read()
OLD_AUDIO_LINE = '    target_sources(butterscotch PRIVATE ${GL_SOURCES})'
NEW_AUDIO_LINE = '''    target_sources(butterscotch PRIVATE ${GL_SOURCES})
    # miniaudio.h pulls in AVFoundation which needs ObjC — compile as ObjC
    set_source_files_properties(
        ${CMAKE_SOURCE_DIR}/src/audio/miniaudio/ma_audio_system.c
        PROPERTIES COMPILE_FLAGS "-x objective-c"
    )'''
if 'COMPILE_FLAGS "-x objective-c"' not in cmake_src and OLD_AUDIO_LINE in cmake_src:
    cmake_src = cmake_src.replace(OLD_AUDIO_LINE, NEW_AUDIO_LINE)
    with open(path, "w") as f:
        f.write(cmake_src)
    print("CMakeLists.txt patched: ma_audio_system.c flagged as ObjC.")

# ══════════════════════════════════════════════════════════════════════════════
# 3. src/gl/gl_renderer.h — extend GLES guard to PLATFORM_IOS
# ══════════════════════════════════════════════════════════════════════════════
patch_file(
    os.path.join(src_dir, "gl", "gl_renderer.h"),
    '#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)\n#include <GLES3/gl3.h>\n#else\n#include <glad/glad.h>\n#endif',
    '#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(PLATFORM_IOS)\n#include <OpenGLES/ES3/gl.h>\n#include <OpenGLES/ES3/glext.h>\n#else\n#include <glad/glad.h>\n#endif',
    'PLATFORM_IOS',
    "OpenGLES guard in header"
)

# ══════════════════════════════════════════════════════════════════════════════
# 4. src/gl/gl_renderer.c — guard glad include
# ══════════════════════════════════════════════════════════════════════════════
patch_file(
    os.path.join(src_dir, "gl", "gl_renderer.c"),
    '#include <glad/glad.h>',
    '#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(PLATFORM_IOS)\n#include <OpenGLES/ES3/gl.h>\n#include <OpenGLES/ES3/glext.h>\n#else\n#include <glad/glad.h>\n#endif',
    'PLATFORM_IOS',
    "OpenGLES guard in .c"
)

# ══════════════════════════════════════════════════════════════════════════════
# 5. src/gl_common/gl_common.c and gl_common.h — guard glad include
# ══════════════════════════════════════════════════════════════════════════════
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
        elif 'PLATFORM_IOS' in content:
            print(f"{fname} already patched (OpenGLES guard).")

# ══════════════════════════════════════════════════════════════════════════════
# 6. src/gl_common/gl_wrappers.h — alias OES/EXT names to core names on iOS
#    On iOS ES3 the core functions are always present; OES/EXT variants don't
#    exist in the SDK at all.  Prepend #define aliases guarded by PLATFORM_IOS.
# ══════════════════════════════════════════════════════════════════════════════
gl_wrappers_path = os.path.join(src_dir, "gl_common", "gl_wrappers.h")
if os.path.exists(gl_wrappers_path):
    with open(gl_wrappers_path) as f:
        content = f.read()
    if 'PLATFORM_IOS' not in content and (
        'glBindVertexArrayOES' in content or 'glGenFramebuffersEXT' in content
    ):
        ios_defines = (
            '#ifdef PLATFORM_IOS\n'
            '/* iOS ES3: core functions always available; alias extension names to core */\n'
            '#define glBindVertexArrayOES        glBindVertexArray\n'
            '#define glGenVertexArraysOES        glGenVertexArrays\n'
            '#define glDeleteVertexArraysOES     glDeleteVertexArrays\n'
            '#define glGenFramebuffersEXT        glGenFramebuffers\n'
            '#define glBindFramebufferEXT        glBindFramebuffer\n'
            '#define glFramebufferTexture2DEXT   glFramebufferTexture2D\n'
            '#define glDeleteFramebuffersEXT     glDeleteFramebuffers\n'
            '#define glCheckFramebufferStatusEXT glCheckFramebufferStatus\n'
            '#define glBlitFramebufferEXT        glBlitFramebuffer\n'
            '#endif /* PLATFORM_IOS */\n\n'
        )
        content = ios_defines + content
        with open(gl_wrappers_path, 'w') as f:
            f.write(content)
        print("gl_wrappers.h patched for iOS (OES/EXT alias defines).")
    elif 'PLATFORM_IOS' in content:
        print("gl_wrappers.h already patched (OES/EXT alias defines).")
else:
    print("Warning: gl_wrappers.h not found — skipping OES/EXT alias patch.")

# ══════════════════════════════════════════════════════════════════════════════
# 7. src/gl/gl_renderer.c — capability-check functions that test EXT/OES ptrs
#    glGenFramebuffersEXT and glGenVertexArraysOES don't exist on iOS ES3.
#    On iOS just return 1 (the core functions are always present).
# ══════════════════════════════════════════════════════════════════════════════
gl_renderer_c_path = os.path.join(src_dir, "gl", "gl_renderer.c")
if os.path.exists(gl_renderer_c_path):
    with open(gl_renderer_c_path) as f:
        renderer_src = f.read()
    changed = False

    OLD_CAP1 = 'return (glGenFramebuffers || glGenFramebuffersEXT);'
    NEW_CAP1 = (
        '#ifdef PLATFORM_IOS\n'
        '    return 1; /* iOS ES3: glGenFramebuffers always present */\n'
        '#else\n'
        '    return (glGenFramebuffers || glGenFramebuffersEXT);\n'
        '#endif'
    )
    if OLD_CAP1 in renderer_src and NEW_CAP1 not in renderer_src:
        renderer_src = renderer_src.replace(OLD_CAP1, NEW_CAP1)
        changed = True

    OLD_CAP2 = 'return (glGenVertexArrays || glGenVertexArraysOES);'
    NEW_CAP2 = (
        '#ifdef PLATFORM_IOS\n'
        '    return 1; /* iOS ES3: glGenVertexArrays always present */\n'
        '#else\n'
        '    return (glGenVertexArrays || glGenVertexArraysOES);\n'
        '#endif'
    )
    if OLD_CAP2 in renderer_src and NEW_CAP2 not in renderer_src:
        renderer_src = renderer_src.replace(OLD_CAP2, NEW_CAP2)
        changed = True

    if changed:
        with open(gl_renderer_c_path, 'w') as f:
            f.write(renderer_src)
        print("gl_renderer.c patched: capability checks made iOS-safe.")
    else:
        print("gl_renderer.c capability checks already patched or not found.")
else:
    print("Warning: gl_renderer.c not found — skipping capability-check patch.")
