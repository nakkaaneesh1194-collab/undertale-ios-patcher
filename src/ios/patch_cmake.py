#!/usr/bin/env python3
# Patches Butterscotch's CMakeLists.txt to add iOS support.
# Safe to run multiple times (checks if already patched).

import sys, re

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
    # SDL2
    find_package(SDL2 REQUIRED)
    target_include_directories(butterscotch PRIVATE ${SDL2_INCLUDE_DIRS})
    # iOS frameworks
    target_link_libraries(butterscotch PRIVATE
        ${SDL2_LIBRARIES}
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
