#!/usr/bin/env bash
# ============================================================
#  undertale-ios-patcher  v2.0
#  Builds Butterscotch for iOS and packages Undertale as an IPA
#
#  Usage:
#    ./patch_ios.sh [path/to/Undertale/install/folder]
#    ./patch_ios.sh -v   (verbose — shows full output from tools)
#
#  Requirements (auto-installed if missing):
#    - Xcode (for iOS SDK + clang)
#    - cmake
#    - UndertaleModCli (for data.win patching)
#    - No SDL2 required (uses UIKit/EAGL directly)
#    - Butterscotch source (cloned automatically)
#
#  Output:
#    Undertale_patched.ipa  (ready for LiveContainer or AltStore)
# ============================================================

set -e

# ---- Verbose flag ----
VERBOSE=false
INSTALL_DIR_ARG=""
for arg in "$@"; do
    case "$arg" in
        -v|--verbose) VERBOSE=true ;;
        *) INSTALL_DIR_ARG="$arg" ;;
    esac
done

# ---- Colors ----
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# ---- Banner ----
clear
echo ""
echo -e "${CYAN}${BOLD}"
echo "  ╔══════════════════════════════════════════╗"
echo "  ║     Undertale iOS Patcher  v2.0          ║"
echo "  ║                                          ║"
echo "  ╚══════════════════════════════════════════╝"
echo -e "${NC}"
$VERBOSE && echo -e "  ${YELLOW}Verbose mode on${NC}\n"

# ---- Helpers ----
info()    { echo -e "  ${CYAN}→${NC} $1"; }
success() { echo -e "  ${GREEN}✓${NC} $1"; }
warn()    { echo -e "  ${YELLOW}⚠${NC}  $1"; }
error()   { echo -e "\n  ${RED}✗ Error:${NC} $1\n"; exit 1; }
section() { echo -e "\n${BOLD}$1${NC}"; }

# Run a command, hiding output unless -v
quietly() {
    if $VERBOSE; then
        "$@"
    else
        "$@" &>/dev/null
    fi
}

ask_yn() {
    local prompt="$1" default="${2:-y}"
    while true; do
        [ "$default" = "y" ] && echo -ne "  ${BOLD}$prompt${NC} [Y/n] " \
                             || echo -ne "  ${BOLD}$prompt${NC} [y/N] "
        read -r reply; reply="${reply:-$default}"
        case "$reply" in [Yy]*) return 0;; [Nn]*) return 1;;
            *) echo "    Please answer y or n.";; esac
    done
}

human_size() {
    local b=$1
    if   [ "$b" -ge 1073741824 ]; then echo "$(( b/1073741824 )) GB"
    elif [ "$b" -ge 1048576 ];    then echo "$(( b/1048576 )) MB"
    else                               echo "$(( b/1024 )) KB"; fi
}

fetch_size() {
    local url="$1" bytes
    bytes=$(curl -sI "$url" 2>/dev/null | grep -i content-length | tail -1 | tr -d '\r' | awk '{print $2}')
    if [ -n "$bytes" ] && [ "$bytes" -gt 0 ] 2>/dev/null; then human_size "$bytes"
    else echo "unknown size"; fi
}

# ---- Script dir ----
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUTTERSCOTCH_DIR="$SCRIPT_DIR/Butterscotch"
UTMT_DIR="$SCRIPT_DIR/UndertaleModCli"
UTMT_BIN="$UTMT_DIR/UndertaleModCli"

# ---- Detect host platform ----
HOST_OS="$(uname -s)"
IS_LINUX=false
IS_MAC=false
case "$HOST_OS" in
    Linux*)  IS_LINUX=true ;;
    Darwin*) IS_MAC=true ;;
    *) error "Unsupported host OS: $HOST_OS" ;;
esac

# ============================================================
# DEPENDENCY CHECKS
# ============================================================
section "Checking dependencies..."

# --- iOS SDK / toolchain ---
TOOLCHAIN_DIR="$HOME/.local/ios-toolchain"
TOOLCHAIN_FILE="$SCRIPT_DIR/tools/ios-linux-toolchain.cmake"
CMAKE_EXTRA_ARGS=""
BUILD_GENERATOR="Unix Makefiles"

if $IS_MAC; then
    if ! xcrun --sdk iphoneos --show-sdk-path &>/dev/null; then
        error "Xcode not found. Install Xcode from the App Store, then run:\n  sudo xcode-select --switch /Applications/Xcode.app"
    fi
    SYSROOT=$(xcrun --sdk iphoneos --show-sdk-path)
    success "iOS SDK: $(basename "$SYSROOT")"
    BUILD_GENERATOR="Xcode"
    CMAKE_EXTRA_ARGS="-DCMAKE_OSX_SYSROOT=$SYSROOT -GXcode"
else
    # Linux — check toolchain is set up
    if [ ! -f "$TOOLCHAIN_FILE" ]; then
        error "iOS toolchain not set up. Run:\n  bash tools/setup_linux_toolchain.sh"
    fi
    SDK_PATH=$(find "$TOOLCHAIN_DIR/sdk" -maxdepth 1 -name "iPhoneOS*.sdk" -type d | head -1)
    [ -n "$SDK_PATH" ] || error "iPhoneOS SDK not found in $TOOLCHAIN_DIR/sdk\nRe-run tools/setup_linux_toolchain.sh"
    SYSROOT="$SDK_PATH"
    success "iOS SDK: $(basename "$SYSROOT")"
    success "Cross-compiler: $TOOLCHAIN_DIR/bin/ios-clang"
    CMAKE_EXTRA_ARGS="-DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN_FILE"
    BUILD_GENERATOR="Unix Makefiles"
fi

# --- cmake ---
if ! command -v cmake &>/dev/null; then
    warn "cmake not found."
    if ask_yn "Install cmake via Homebrew? (~20 MB)"; then
        if ! command -v brew &>/dev/null; then
            error "Homebrew not found. Install it from https://brew.sh then re-run."
        fi
        info "Installing cmake..."
        HOMEBREW_NO_AUTO_UPDATE=1 quietly brew install --quiet cmake \
            || error "Failed to install cmake via Homebrew."
        success "cmake installed"
    else
        error "cmake is required. Install it from https://cmake.org or via Homebrew."
    fi
fi
success "cmake: $(cmake --version | head -1)"

# --- UndertaleModCli ---
if [ ! -x "$UTMT_BIN" ]; then
    warn "UndertaleModCli not found."
    # Try .zip (macOS) first, fall back to .tar.gz (Linux)
    UTMT_BASE="https://github.com/UnderminersTeam/UndertaleModTool/releases/download/0.9.1.1"
    UTMT_URL_ZIP="$UTMT_BASE/UTMT_CLI_v0.9.1.1-macOS.zip"
    UTMT_URL_TAR="$UTMT_BASE/UTMT_CLI_v0.9.1.1-Linux.tar.gz"
    UTMT_SIZE=$(fetch_size "$UTMT_URL_ZIP")
    [ "$UTMT_SIZE" = "unknown size" ] && UTMT_SIZE=$(fetch_size "$UTMT_URL_TAR")
    if ask_yn "Download UndertaleModCli? (~$UTMT_SIZE)"; then
        info "Downloading UndertaleModCli..."
        mkdir -p "$UTMT_DIR"
        TMP_FILE=$(mktemp /tmp/utmt.XXXXXX)
        # Try .zip first
        if curl -sf -L --progress-bar "$UTMT_URL_ZIP" -o "${TMP_FILE}.zip" 2>/dev/null && \
           [ -s "${TMP_FILE}.zip" ]; then
            info "Extracting..."
            quietly unzip -o "${TMP_FILE}.zip" -d "$UTMT_DIR"
            rm -f "${TMP_FILE}.zip"
        else
            # Fall back to .tar.gz
            curl -L --progress-bar "$UTMT_URL_TAR" -o "${TMP_FILE}.tar.gz" || error "Download failed."
            info "Extracting..."
            quietly tar -xzf "${TMP_FILE}.tar.gz" -C "$UTMT_DIR" --strip-components=1
            rm -f "${TMP_FILE}.tar.gz"
        fi
        rm -f "$TMP_FILE"
        # Find the binary — it may be nested or named differently
        chmod +x "$UTMT_BIN" 2>/dev/null || true
        if [ ! -x "$UTMT_BIN" ]; then
            FOUND=$(find "$UTMT_DIR" -name "UndertaleModCli" -not -name "*.dll" -not -name "*.so" -type f | head -1)
            if [ -z "$FOUND" ]; then
                # Sometimes the zip extracts a folder with the version in the name
                FOUND=$(find "$UTMT_DIR" -maxdepth 3 -type f -perm +111 -name "UndertaleModCli*" | head -1)
            fi
            [ -n "$FOUND" ] && chmod +x "$FOUND" && ln -sf "$FOUND" "$UTMT_BIN" \
                || error "UndertaleModCli binary not found after extraction. Run with -v for details."
        fi
        success "UndertaleModCli installed"
    else
        error "UndertaleModCli is required."
    fi
else
    success "UndertaleModCli: found"
fi

# --- git ---
if ! command -v git &>/dev/null; then
    error "git not found. Install Xcode Command Line Tools: xcode-select --install"
fi

# --- Butterscotch source ---
if [ ! -f "$BUTTERSCOTCH_DIR/CMakeLists.txt" ]; then
    warn "Butterscotch source not found."
    if ask_yn "Clone Butterscotch automatically? (~30 MB)"; then
        info "Cloning Butterscotch..."
        BS_ZIP=$(mktemp /tmp/butterscotch.XXXXXX).zip
        curl -fsSL --progress-bar \
            "https://github.com/ButterscotchRunner/Butterscotch/archive/refs/heads/main.zip" \
            -o "$BS_ZIP" || error "Failed to download Butterscotch. Check your internet connection."
        mkdir -p "$BUTTERSCOTCH_DIR"
        quietly unzip -o "$BS_ZIP" -d /tmp/bs-extract
        # zip extracts as Butterscotch-main/
        cp -R /tmp/bs-extract/Butterscotch-main/. "$BUTTERSCOTCH_DIR/"
        rm -rf "$BS_ZIP" /tmp/bs-extract
        success "Butterscotch downloaded"
    else
        error "Butterscotch source is required.\n  git clone https://github.com/PerfectDreams/Butterscotch $BUTTERSCOTCH_DIR"
    fi
else
    success "Butterscotch source: found"
fi

# --- Apply iOS patch to Butterscotch (always copy so edits take effect) ---
IOS_MARKER="$BUTTERSCOTCH_DIR/src/ios/main.c"
if [ ! -f "$IOS_MARKER" ]; then
    info "Patching Butterscotch CMakeLists.txt..."
    python3 "$SCRIPT_DIR/src/ios/patch_cmake.py" "$BUTTERSCOTCH_DIR/CMakeLists.txt" \
        || error "Failed to patch Butterscotch CMakeLists.txt."
fi
mkdir -p "$BUTTERSCOTCH_DIR/src/ios"
cp "$SCRIPT_DIR/src/ios/"* "$BUTTERSCOTCH_DIR/src/ios/" \
    || error "Failed to copy src/ios/ into Butterscotch."
success "iOS sources synced to Butterscotch"

# Fix gl_wrappers.h: remove desktop-only EXT fallbacks that don't exist on OpenGL ES
GL_WRAPPERS="$BUTTERSCOTCH_DIR/src/gl_common/gl_wrappers.h"
if [ -f "$GL_WRAPPERS" ]; then
    sed -i 's/    else glBlendEquationEXT(mode);/    \/* glBlendEquationEXT not on GLES *\//' "$GL_WRAPPERS"
    sed -i 's/    else glBlendFuncSeparateEXT(sfactorRGB, dfactorRGB, sfactorAlpha, dfactorAlpha);/    \/* glBlendFuncSeparateEXT not on GLES *\//' "$GL_WRAPPERS"
fi

# No SDL2 needed — using UIKit/EAGL directly

echo ""

# ============================================================
# INPUT FILES
# ============================================================
section "Input files"

INSTALL_DIR="$INSTALL_DIR_ARG"
if [ -z "$INSTALL_DIR" ]; then
    STEAM_CANDIDATES=(
        "$HOME/Library/Application Support/Steam/steamapps/common/Undertale"
        "$HOME/Library/Application Support/Steam/steamapps/common/UNDERTALE"
    )
    for candidate in "${STEAM_CANDIDATES[@]}"; do
        if [ -f "$candidate/data.win" ] || [ -f "$candidate/game.ios" ]; then
            INSTALL_DIR="$candidate"
            info "Found Undertale install at: $INSTALL_DIR"
            break
        fi
    done
    if [ -z "$INSTALL_DIR" ]; then
        echo -ne "  ${BOLD}Path to Undertale install folder (Steam):${NC} "
        read -r INSTALL_DIR
    fi
fi

INSTALL_DIR="${INSTALL_DIR%/}"
INSTALL_DIR="${INSTALL_DIR/#\~/$HOME}"
[ -d "$INSTALL_DIR" ] || error "Folder not found: $INSTALL_DIR"

DATAWIN=""
for candidate in "$INSTALL_DIR/data.win" "$INSTALL_DIR/game.ios" "$INSTALL_DIR/assets/data.win"; do
    if [ -f "$candidate" ]; then
        DATAWIN="$candidate"
        break
    fi
done
[ -n "$DATAWIN" ] || error "Could not find data.win in: $INSTALL_DIR"
success "Install folder: $INSTALL_DIR"
success "data.win: $(basename "$DATAWIN")"
echo ""

# ============================================================
# OPTIONS
# ============================================================
section "Options"

USE_THUMBSTICK_FLAG=""
if ask_yn "Use D-Pad for movement? (No = analog thumbstick)" y; then
    success "Input: D-Pad"
else
    USE_THUMBSTICK_FLAG="-DVIRTUAL_CONTROLLER_THUMBSTICK=ON"
    warn "Input: Analog thumbstick"
fi

DEBUG_OVERLAY=false
if ask_yn "Add debug overlay? (shows gamepad info, useful for troubleshooting)" n; then
    DEBUG_OVERLAY=true
    warn "Debug overlay: enabled"
fi

IPA_OUT="$SCRIPT_DIR/Undertale_patched.ipa"
echo -ne "\n  ${BOLD}Output IPA path${NC} [${IPA_OUT}]: "
read -r custom_out
[ -n "$custom_out" ] && IPA_OUT="$custom_out"

echo ""

# ============================================================
# PATCH data.win
# ============================================================
section "Patching data.win..."

WORK_DIR=$(mktemp -d)
PATCHED_DATAWIN="$WORK_DIR/data.win"
cp "$DATAWIN" "$PATCHED_DATAWIN"

PATCHES_DIR="$SCRIPT_DIR/patches"

run_patch() {
    local script="$1" label="$2"
    info "Applying $label..."
    if $VERBOSE; then
        "$UTMT_BIN" load "$PATCHED_DATAWIN" -s "$script" -o "$PATCHED_DATAWIN"
    else
        "$UTMT_BIN" load "$PATCHED_DATAWIN" -s "$script" -o "$PATCHED_DATAWIN" 2>&1 \
            | grep -v "^\[MESSAGE\]" | grep -iE "error:|failed" || true
    fi
}

run_patch "$PATCHES_DIR/01_jch_init.csx"       "01 — j_ch init"
run_patch "$PATCHES_DIR/02_jch_rescan.csx"     "02 — j_ch rescan"
run_patch "$PATCHES_DIR/03_control_update.csx" "03 — control_update device index"
run_patch "$PATCHES_DIR/04_dpad_hold.csx"      "04 — d-pad hold"
run_patch "$PATCHES_DIR/05_title_screen.csx"   "05 — title screen button check"
$DEBUG_OVERLAY && run_patch "$PATCHES_DIR/06_debug_overlay.csx" "06 — debug overlay"

success "data.win patched"

# ============================================================
# BUILD BUTTERSCOTCH FOR IOS
# ============================================================
section "Building Butterscotch for iOS..."

BUILD_DIR="$WORK_DIR/build-ios"

info "Configuring CMake..."
CMAKE_LOG=$(mktemp /tmp/cmake-configure.XXXXXX)
cmake -S "$BUTTERSCOTCH_DIR" -B "$BUILD_DIR" \
    -DPLATFORM=ios \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
    -DENABLE_WAD16=ON \
    -DENABLE_WAD14=OFF \
    -DENABLE_WAD17=OFF \
    -DENABLE_LEGACY_GL=OFF \
    -DENABLE_MODERN_GL=ON \
    ${USE_THUMBSTICK_FLAG} \
    -DCMAKE_BUILD_TYPE=Release \
    -G"$BUILD_GENERATOR" \
    $CMAKE_EXTRA_ARGS >"$CMAKE_LOG" 2>&1 \
    || { cat "$CMAKE_LOG"; rm -f "$CMAKE_LOG"; error "CMake configure failed."; }
$VERBOSE && cat "$CMAKE_LOG" || grep -iE "error:|warning:" "$CMAKE_LOG" | tail -5 || true
rm -f "$CMAKE_LOG"

info "Compiling (this may take a few minutes)..."
BUILD_LOG=$(mktemp /tmp/cmake-build.XXXXXX)
if $IS_MAC; then
    cmake --build "$BUILD_DIR" \
        --config Release \
        -- \
        CODE_SIGNING_ALLOWED=NO \
        CODE_SIGNING_REQUIRED=NO \
        CODE_SIGN_IDENTITY="" >"$BUILD_LOG" 2>&1 \
        || { cat "$BUILD_LOG"; rm -f "$BUILD_LOG"; error "Build failed."; }
else
    cmake --build "$BUILD_DIR" \
        --config Release \
        -j$(nproc) >"$BUILD_LOG" 2>&1 \
        || { cat "$BUILD_LOG"; rm -f "$BUILD_LOG"; error "Build failed."; }
fi
$VERBOSE && cat "$BUILD_LOG" || grep -iE "error:|Build succeeded|FAILED" "$BUILD_LOG" | tail -5 || true
rm -f "$BUILD_LOG"

APP_PATH=$(find "$BUILD_DIR" -name "butterscotch.app" -type d | head -1)
# On Linux the binary is just called "butterscotch" with no .app wrapper — create one
if [ -z "$APP_PATH" ]; then
    BS_BIN=$(find "$BUILD_DIR" -name "butterscotch" -type f | head -1)
    [ -n "$BS_BIN" ] || error "Build failed — butterscotch binary not found.\nRun with -v for full output."
    APP_PATH="$BUILD_DIR/butterscotch.app"
    mkdir -p "$APP_PATH"
    cp "$BS_BIN" "$APP_PATH/butterscotch"
    # fake-sign the binary
    if command -v ldid &>/dev/null; then
        ldid -S "$APP_PATH/butterscotch"
    elif [ -f "$HOME/.local/ios-toolchain/bin/ldid" ]; then
        "$HOME/.local/ios-toolchain/bin/ldid" -S "$APP_PATH/butterscotch"
    fi
fi
success "Build complete"

# ============================================================
# BUNDLE GAME FILES INTO APP
# ============================================================
section "Bundling game files..."

# On Linux, copy Info.plist into the .app (Xcode does this automatically on Mac)
if $IS_LINUX && [ ! -f "$APP_PATH/Info.plist" ]; then
    PLIST_SRC=$(find "$BUTTERSCOTCH_DIR/src/ios" -name "Info.plist*" | head -1)
    if [ -n "$PLIST_SRC" ]; then
        # Substitute CMake variables in the .in template if needed
        sed "s/@MACOSX_BUNDLE_GUI_IDENTIFIER@/org.butterscotch.Undertale/g;
             s/@MACOSX_BUNDLE_BUNDLE_NAME@/Undertale/g;
             s/@MACOSX_BUNDLE_SHORT_VERSION_STRING@/1.0/g;
             s/@MACOSX_BUNDLE_BUNDLE_VERSION@/1/g" \
            "$PLIST_SRC" > "$APP_PATH/Info.plist"
    fi
fi

info "Copying game assets..."
if command -v rsync &>/dev/null; then
    quietly rsync -a \
        --exclude="*.app" --exclude="*.exe" --exclude="*.dll" \
        --exclude="*.sh" --exclude=".DS_Store" \
        "$INSTALL_DIR/" "$APP_PATH/"
else
    cp -R "$INSTALL_DIR"/. "$APP_PATH/"
fi
cp "$PATCHED_DATAWIN" "$APP_PATH/data.win"
success "Game assets bundled"

# ============================================================
# PACKAGE AS IPA
# ============================================================
section "Packaging IPA..."

IPA_WORK="$WORK_DIR/ipa"
mkdir -p "$IPA_WORK/Payload"
cp -R "$APP_PATH" "$IPA_WORK/Payload/"
(cd "$IPA_WORK" && quietly zip -r "$IPA_OUT" Payload)

success "IPA → $IPA_OUT"
info "Size: $(du -sh "$IPA_OUT" | cut -f1)"

rm -rf "$WORK_DIR"

# ============================================================
# DONE
# ============================================================
echo ""
echo -e "${GREEN}${BOLD}  ✓ All done!${NC}"
echo ""
echo -e "  Patched IPA: ${BOLD}$IPA_OUT${NC}"
echo ""
echo "  Next steps:"
echo "   1. Sign the IPA with AltStore, SideStore, or Sideloadly"
echo "   2. Or load directly in LiveContainer (no signing needed)"
echo "   3. Launch — the virtual controller appears automatically"
echo ""
