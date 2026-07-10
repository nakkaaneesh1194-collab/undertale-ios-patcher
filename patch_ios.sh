#!/usr/bin/env bash
# ============================================================
#  undertale-ios-patcher  v2.0
#  Builds Butterscotch for iOS and packages Undertale as an IPA
#
#  Usage:
#    ./patch_ios.sh [path/to/Undertale/install/folder]
#
#  Requirements (auto-installed if missing):
#    - Xcode (for iOS SDK + clang)
#    - cmake
#    - UndertaleModCli (for data.win patching)
#    - SDL2 (downloaded automatically)
#    - Butterscotch source (bundled with this script or in ./Butterscotch/)
#
#  Output:
#    Undertale_patched.ipa  (ready for LiveContainer or AltStore)
# ============================================================

set -e

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
echo "  ║   Butterscotch + GCVirtualController     ║"
echo "  ╚══════════════════════════════════════════╝"
echo -e "${NC}"

# ---- Helpers ----
info()    { echo -e "  ${CYAN}→${NC} $1"; }
success() { echo -e "  ${GREEN}✓${NC} $1"; }
warn()    { echo -e "  ${YELLOW}⚠${NC}  $1"; }
error()   { echo -e "\n  ${RED}✗ Error:${NC} $1\n"; exit 1; }
section() { echo -e "\n${BOLD}$1${NC}"; }

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
SDL2_DIR="$SCRIPT_DIR/SDL2-ios"

# ============================================================
# DEPENDENCY CHECKS
# ============================================================
section "Checking dependencies..."

# --- Xcode / iOS SDK ---
if ! xcrun --sdk iphoneos --show-sdk-path &>/dev/null; then
    error "Xcode not found. Install Xcode from the App Store, then run:\n  sudo xcode-select --switch /Applications/Xcode.app"
fi
SYSROOT=$(xcrun --sdk iphoneos --show-sdk-path)
success "iOS SDK: $(basename "$SYSROOT")"

# --- cmake ---
if ! command -v cmake &>/dev/null; then
    warn "cmake not found."
    CMAKE_SIZE=$(fetch_size "https://github.com/Kitware/CMake/releases/latest")
    if ask_yn "Install cmake via Homebrew? (~10 MB)"; then
        if ! command -v brew &>/dev/null; then
            error "Homebrew not found. Install it from https://brew.sh then re-run."
        fi
        brew install cmake
    else
        error "cmake is required. Install it from https://cmake.org or via Homebrew."
    fi
fi
success "cmake: $(cmake --version | head -1)"

# --- UndertaleModCli ---
if [ ! -x "$UTMT_BIN" ]; then
    warn "UndertaleModCli not found."
    UTMT_URL="https://github.com/UnderminersTeam/UndertaleModTool/releases/download/0.9.1.1/UndertaleModCli_macOS.tar.gz"
    UTMT_SIZE=$(fetch_size "$UTMT_URL")
    if ask_yn "Download UndertaleModCli? (~$UTMT_SIZE)"; then
        info "Downloading UndertaleModCli..."
        mkdir -p "$UTMT_DIR"
        TMP_TAR=$(mktemp /tmp/utmt.XXXXXX.tar.gz)
        curl -L --progress-bar "$UTMT_URL" -o "$TMP_TAR" || error "Download failed."
        tar -xzf "$TMP_TAR" -C "$UTMT_DIR" --strip-components=1 2>/dev/null || tar -xzf "$TMP_TAR" -C "$UTMT_DIR"
        rm -f "$TMP_TAR"
        chmod +x "$UTMT_BIN" 2>/dev/null || true
        if [ ! -x "$UTMT_BIN" ]; then
            FOUND=$(find "$UTMT_DIR" -name "UndertaleModCli" -type f | head -1)
            [ -n "$FOUND" ] && ln -sf "$FOUND" "$UTMT_BIN" || error "UndertaleModCli binary not found after extraction."
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
        git clone --depth=1 https://github.com/PerfectDreams/Butterscotch "$BUTTERSCOTCH_DIR" \
            || error "Failed to clone Butterscotch. Check your internet connection."
        success "Butterscotch cloned"
    else
        error "Butterscotch source is required.\n  git clone https://github.com/PerfectDreams/Butterscotch $BUTTERSCOTCH_DIR"
    fi
else
    success "Butterscotch source: found"
fi

# --- Apply iOS patch to Butterscotch if not already applied ---
IOS_MARKER="$BUTTERSCOTCH_DIR/src/ios/main.c"
if [ ! -f "$IOS_MARKER" ]; then
    info "Applying iOS patch to Butterscotch..."

    # Apply CMakeLists.patch
    if [ -f "$SCRIPT_DIR/CMakeLists.patch" ]; then
        cd "$BUTTERSCOTCH_DIR"
        if git apply --check "$SCRIPT_DIR/CMakeLists.patch" &>/dev/null; then
            git apply "$SCRIPT_DIR/CMakeLists.patch" \
                || error "Failed to apply CMakeLists.patch."
        else
            warn "CMakeLists.patch did not apply cleanly — Butterscotch may have updated."
            warn "You may need to manually apply CMakeLists.patch to Butterscotch/CMakeLists.txt."
        fi
        cd "$SCRIPT_DIR"
    else
        error "CMakeLists.patch not found next to patch_ios.sh."
    fi

    # Copy src/ios/ platform files
    mkdir -p "$BUTTERSCOTCH_DIR/src/ios"
    cp "$SCRIPT_DIR/src/ios/"* "$BUTTERSCOTCH_DIR/src/ios/" \
        || error "Failed to copy src/ios/ into Butterscotch."

    success "iOS patch applied to Butterscotch"
else
    success "Butterscotch iOS patch: already applied"
fi

# --- SDL2 xcframework ---
SDL2_XCFW="$SDL2_DIR/SDL2.xcframework"
if [ ! -d "$SDL2_XCFW" ]; then
    warn "SDL2.xcframework not found."
    SDL2_URL="https://github.com/libsdl-org/SDL/releases/download/release-2.30.3/SDL2-2.30.3.dmg"
    SDL2_SIZE=$(fetch_size "$SDL2_URL")
    if ask_yn "Download SDL2 for iOS? (~$SDL2_SIZE)"; then
        info "Downloading SDL2..."
        TMP_DMG=$(mktemp /tmp/SDL2.XXXXXX.dmg)
        curl -L --progress-bar "$SDL2_URL" -o "$TMP_DMG" || error "SDL2 download failed."
        info "Mounting SDL2 disk image..."
        MOUNT_POINT=$(mktemp -d)
        hdiutil attach "$TMP_DMG" -mountpoint "$MOUNT_POINT" -quiet
        mkdir -p "$SDL2_DIR"
        cp -R "$MOUNT_POINT"/SDL2.xcframework "$SDL2_DIR/" 2>/dev/null || \
            cp -R "$MOUNT_POINT"/SDL2*/SDL2.xcframework "$SDL2_DIR/" 2>/dev/null || \
            error "Could not find SDL2.xcframework in the disk image."
        hdiutil detach "$MOUNT_POINT" -quiet
        rm -rf "$MOUNT_POINT" "$TMP_DMG"
        success "SDL2 installed → $SDL2_XCFW"
    else
        error "SDL2 is required. Download SDL2-*.dmg from https://github.com/libsdl-org/SDL/releases and place SDL2.xcframework at:\n  $SDL2_XCFW"
    fi
else
    success "SDL2.xcframework: found"
fi

echo ""

# ============================================================
# INPUT FILES
# ============================================================
section "Input files"

INSTALL_DIR="${1:-}"
if [ -z "$INSTALL_DIR" ]; then
    # Try common Steam install locations on Mac
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

# Strip trailing slash
INSTALL_DIR="${INSTALL_DIR%/}"
[ -d "$INSTALL_DIR" ] || error "Folder not found: $INSTALL_DIR"

# Find data.win inside the install folder
DATAWIN=""
for candidate in "$INSTALL_DIR/data.win" "$INSTALL_DIR/game.ios" "$INSTALL_DIR/assets/data.win"; do
    if [ -f "$candidate" ]; then
        DATAWIN="$candidate"
        break
    fi
done
[ -n "$DATAWIN" ] || error "Could not find data.win in: $INSTALL_DIR"
success "Install folder: $INSTALL_DIR"
success "data.win: $DATAWIN"
echo ""

# ============================================================
# OPTIONS
# ============================================================
section "Options"

USE_DPAD=true
USE_THUMBSTICK_FLAG=""
if ask_yn "Use D-Pad for movement? (No = analog thumbstick)" y; then
    USE_DPAD=true
    success "Input: D-Pad"
else
    USE_DPAD=false
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
    "$UTMT_BIN" load "$PATCHED_DATAWIN" -s "$script" -o "$PATCHED_DATAWIN" 2>&1 \
        | grep -v "^\[MESSAGE\]" | grep -v "^$" || true
}

run_patch "$PATCHES_DIR/01_jch_init.csx"       "01 — j_ch init"
run_patch "$PATCHES_DIR/02_jch_rescan.csx"     "02 — j_ch rescan"
run_patch "$PATCHES_DIR/03_control_update.csx" "03 — control_update device index"
run_patch "$PATCHES_DIR/04_dpad_hold.csx"      "04 — d-pad hold"
run_patch "$PATCHES_DIR/05_title_screen.csx"   "05 — title screen button check"

if $DEBUG_OVERLAY; then
    run_patch "$PATCHES_DIR/06_debug_overlay.csx" "06 — debug overlay"
fi

success "data.win patched"

# ============================================================
# BUILD BUTTERSCOTCH FOR IOS
# ============================================================
section "Building Butterscotch for iOS..."

BUILD_DIR="$WORK_DIR/build-ios"
SDL2_CMAKE_DIR="$SDL2_XCFW/ios-arm64/SDL2.framework/Resources/CMake"
# Fallback cmake dir paths for different SDL2 xcframework layouts
if [ ! -d "$SDL2_CMAKE_DIR" ]; then
    SDL2_CMAKE_DIR=$(find "$SDL2_XCFW" -name "SDL2Config.cmake" -o -name "sdl2-config.cmake" 2>/dev/null | head -1 | xargs dirname 2>/dev/null || echo "")
fi
if [ ! -d "$SDL2_CMAKE_DIR" ]; then
    SDL2_CMAKE_DIR=$(find "$SDL2_DIR" -name "SDL2Config.cmake" 2>/dev/null | head -1 | xargs dirname 2>/dev/null || echo "")
fi

info "Configuring CMake..."
cmake -S "$BUTTERSCOTCH_DIR" -B "$BUILD_DIR" \
    -DPLATFORM=ios \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
    -DCMAKE_OSX_SYSROOT="$SYSROOT" \
    -DENABLE_WAD16=ON \
    -DENABLE_WAD14=OFF \
    -DENABLE_WAD17=OFF \
    -DENABLE_LEGACY_GL=OFF \
    -DENABLE_MODERN_GL=ON \
    ${USE_THUMBSTICK_FLAG} \
    ${SDL2_CMAKE_DIR:+-DSDL2_DIR="$SDL2_CMAKE_DIR"} \
    -DCMAKE_BUILD_TYPE=Release \
    -GXcode \
    2>&1 | tail -5

info "Compiling (this may take a few minutes)..."
cmake --build "$BUILD_DIR" \
    --config Release \
    -- \
    CODE_SIGNING_ALLOWED=NO \
    CODE_SIGNING_REQUIRED=NO \
    CODE_SIGN_IDENTITY="" \
    2>&1 | grep -E "error:|warning:|Build succeeded|FAILED" | tail -20

APP_PATH=$(find "$BUILD_DIR" -name "butterscotch.app" -type d | head -1)
[ -n "$APP_PATH" ] || error "Build failed — butterscotch.app not found in $BUILD_DIR"
success "Built: $APP_PATH"

# ============================================================
# BUNDLE GAME FILES INTO APP
# ============================================================
section "Bundling game files..."

# Copy all game assets from the install folder into the .app bundle,
# then overwrite data.win with the patched version.
#
# Undertale's install contains: data.win, audiogroup*.dat, *.ogg audio,
# fonts, splash images, etc. We copy everything except the Mac .app wrapper
# itself and any platform-specific executables.
info "Copying game assets from install folder..."
rsync -a --exclude="*.app" --exclude="*.exe" --exclude="*.dll" \
    --exclude="*.sh" --exclude=".DS_Store" \
    "$INSTALL_DIR/" "$APP_PATH/" \
    2>/dev/null || \
cp -R "$INSTALL_DIR"/. "$APP_PATH/"

# Overwrite data.win with patched version
cp "$PATCHED_DATAWIN" "$APP_PATH/data.win"
success "Game assets bundled → $(basename "$APP_PATH")"

# ============================================================
# PACKAGE AS IPA
# ============================================================
section "Packaging IPA..."

IPA_WORK="$WORK_DIR/ipa"
mkdir -p "$IPA_WORK/Payload"
cp -R "$APP_PATH" "$IPA_WORK/Payload/"
(cd "$IPA_WORK" && zip -qr "$IPA_OUT" Payload)

success "IPA → $IPA_OUT"
IPA_SIZE=$(du -sh "$IPA_OUT" | cut -f1)
info "Size: $IPA_SIZE"

# ---- Cleanup ----
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
echo "   1. Sign the IPA with AltStore, Sideloadly, or your developer cert"
echo "   2. Import the signed IPA into LiveContainer"
echo "   3. Launch Undertale — the virtual controller appears automatically"
echo ""
echo -e "  ${YELLOW}Note:${NC} No dylib injection needed — GCVirtualController is"
echo "  built directly into Butterscotch."
echo ""
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  