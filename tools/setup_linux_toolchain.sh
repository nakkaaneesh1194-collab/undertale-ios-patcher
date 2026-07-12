#!/usr/bin/env bash
# setup_linux_toolchain.sh — run ONCE on Linux/WSL to set up the iOS cross-compiler.
#
# No Mac required — downloads the iPhoneOS SDK from xybp888/iOS-SDKs on GitHub.
#
# What this installs:
#   - clang/llvm (arm64-apple-ios cross-compilation)
#   - cctools-port (ld64 Mach-O linker)
#   - ldid (fake codesigning)
#   - iPhoneOS26.5.sdk (from xybp888/iOS-SDKs)
#
# Everything goes to ~/.local/ios-toolchain (no sudo needed after apt)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLCHAIN_DIR="$HOME/.local/ios-toolchain"
SDK_VERSION="26.5"
SDK_NAME="iPhoneOS${SDK_VERSION}.sdk"
# Tag archive of the full repo — SDK files are git-tracked, so .tbd stubs are intact on Linux
SDK_URL="https://github.com/xybp888/iOS-SDKs/archive/refs/tags/iOS${SDK_VERSION}-SDKs.tar.gz"
SDK_ARCHIVE_PREFIX="iOS-SDKs-iOS${SDK_VERSION}-SDKs"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

info()    { echo -e "  ${CYAN}→${NC} $1"; }
success() { echo -e "  ${GREEN}✓${NC} $1"; }
warn()    { echo -e "  ${YELLOW}⚠${NC}  $1"; }
error()   { echo -e "\n  ${RED}✗ Error:${NC} $1\n"; exit 1; }

echo ""
echo -e "${CYAN}${BOLD}  iOS Cross-Compiler Setup${NC}"
echo ""

# ── Install system packages ────────────────────────────────────
info "Installing build dependencies..."
sudo apt-get update -qq
sudo apt-get install -y -qq \
    clang llvm lld \
    g++ make cmake \
    git pkg-config \
    libssl-dev uuid-dev \
    libplist-dev \
    zip unzip curl \
    python3 \
    2>/dev/null
success "System packages installed"

# ── Create toolchain dir ───────────────────────────────────────
mkdir -p "$TOOLCHAIN_DIR"/{bin,sdk}

# ── Download SDK ───────────────────────────────────────────────
SDK_PATH="$TOOLCHAIN_DIR/sdk/$SDK_NAME"
SDK_TARBALL="$TOOLCHAIN_DIR/sdk/$SDK_NAME.tar.gz"
LOCAL_TARBALL="$SCRIPT_DIR/iPhoneOS.sdk.tar.gz"
mkdir -p "$TOOLCHAIN_DIR/sdk"

SDK_PATH="$TOOLCHAIN_DIR/sdk/$SDK_NAME"

if [ ! -d "$SDK_PATH" ]; then
    if [ -f "$LOCAL_TARBALL" ]; then
        # Use locally provided tarball (from get_sdk.sh on a Mac) — extract directly
        info "Using local SDK tarball: $LOCAL_TARBALL"
        tar -xzf "$LOCAL_TARBALL" -C "$TOOLCHAIN_DIR/sdk/"
        # get_sdk.sh tars just the SDK folder, so it extracts as iPhoneOS*.sdk/
        EXTRACTED=$(find "$TOOLCHAIN_DIR/sdk" -maxdepth 1 -name "iPhoneOS*.sdk" -type d | head -1)
        [ -n "$EXTRACTED" ] || error "SDK not found after extracting local tarball"
        SDK_PATH="$EXTRACTED"
        success "SDK ready: $(basename "$SDK_PATH")"
    else
        # Download the repo tag archive — SDK is a regular git-tracked directory,
        # so .tbd stub files are intact (no macOS resource fork issues).
        info "Downloading iOS SDK from xybp888/iOS-SDKs..."
        TMP_TAR=$(mktemp /tmp/iPhoneOS.sdk.XXXXXX.tar.gz)
        curl -fL --progress-bar "$SDK_URL" -o "$TMP_TAR" \
            || error "Failed to download SDK.\nFor a complete SDK, run tools/get_sdk.sh on a Mac and copy iPhoneOS.sdk.tar.gz to tools/"

        # The tag archive extracts as iOS-SDKs-iOS26.5-SDKs/iPhoneOS26.5.sdk/...
        # Strip the top-level repo folder so we get just the SDK directory
        info "Extracting SDK..."
        TMP_EXTRACT=$(mktemp -d /tmp/sdk-extract.XXXXXX)
        tar -xzf "$TMP_TAR" -C "$TMP_EXTRACT" \
            "${SDK_ARCHIVE_PREFIX}/${SDK_NAME}/" 2>/dev/null \
            || tar -xzf "$TMP_TAR" -C "$TMP_EXTRACT" 2>/dev/null \
            || error "Failed to extract SDK"
        rm -f "$TMP_TAR"

        # Move the SDK folder to toolchain dir
        FOUND_SDK=$(find "$TMP_EXTRACT" -maxdepth 2 -name "iPhoneOS*.sdk" -type d | head -1)
        [ -n "$FOUND_SDK" ] || error "iPhoneOS*.sdk not found in downloaded archive"
        mv "$FOUND_SDK" "$TOOLCHAIN_DIR/sdk/"
        rm -rf "$TMP_EXTRACT"

        SDK_PATH=$(find "$TOOLCHAIN_DIR/sdk" -maxdepth 1 -name "iPhoneOS*.sdk" -type d | head -1)
        success "SDK ready: $(basename "$SDK_PATH")"
    fi
else
    success "SDK already present: $(basename "$SDK_PATH")"
fi

# ── Build cctools-port (ld64 + Apple binutils) ─────────────────
if [ ! -f "$TOOLCHAIN_DIR/bin/arm-apple-darwin-ld" ]; then
    info "Building cctools-port (Apple Mach-O linker)..."
    # cctools-port's build.sh parses the iOS version from the SDK folder name.
    # Rename the SDK temporarily to match what it expects if needed.
    REAL_SDK=$(find "$TOOLCHAIN_DIR/sdk" -maxdepth 1 -name "iPhoneOS*.sdk" -type d | head -1)
    [ -n "$REAL_SDK" ] || error "SDK not found in $TOOLCHAIN_DIR/sdk"
    SDK_PATH="$REAL_SDK"
    CCTOOLS_SRC=$(mktemp -d /tmp/cctools.XXXXXX)
    git clone --depth=1 https://github.com/tpoechtrager/cctools-port "$CCTOOLS_SRC" \
        || error "Failed to clone cctools-port"
    cd "$CCTOOLS_SRC/usage_examples/ios_toolchain"
    # build.sh expects a SDK tarball as first arg, install prefix as second, arch as third
    bash build.sh "$SDK_TARBALL" "$TOOLCHAIN_DIR" arm 2>&1 \
        || error "cctools-port build failed"
    cd "$SCRIPT_DIR"
    rm -rf "$CCTOOLS_SRC"
    success "cctools-port built"
else
    success "cctools-port: already built"
fi

# ── Build ldid (fake codesigning) ─────────────────────────────
if ! command -v ldid &>/dev/null && [ ! -f "$TOOLCHAIN_DIR/bin/ldid" ]; then
    info "Building ldid..."
    LDID_SRC=$(mktemp -d /tmp/ldid.XXXXXX)
    git clone --depth=1 https://github.com/ProcursusTeam/ldid "$LDID_SRC" \
        || error "Failed to clone ldid"
    cd "$LDID_SRC"
    make -j$(nproc) 2>&1 | tail -5 || error "ldid build failed"
    cp ldid "$TOOLCHAIN_DIR/bin/ldid"
    cd "$SCRIPT_DIR"
    rm -rf "$LDID_SRC"
    success "ldid built"
else
    success "ldid: found"
fi

# ── Write compiler wrapper scripts ────────────────────────────
info "Writing compiler wrappers..."

cat > "$TOOLCHAIN_DIR/bin/ios-clang" << WRAPPER
#!/bin/sh
exec clang \\
    -target arm64-apple-ios15.0 \\
    -isysroot "$SDK_PATH" \\
    -I"$SDK_PATH/usr/include" \\
    "\$@"
WRAPPER
chmod +x "$TOOLCHAIN_DIR/bin/ios-clang"

cat > "$TOOLCHAIN_DIR/bin/ios-clang++" << WRAPPER
#!/bin/sh
exec clang++ \\
    -target arm64-apple-ios15.0 \\
    -isysroot "$SDK_PATH" \\
    -I"$SDK_PATH/usr/include" \\
    "\$@"
WRAPPER
chmod +x "$TOOLCHAIN_DIR/bin/ios-clang++"

success "Compiler wrappers written"

# ── Write CMake toolchain file ─────────────────────────────────
info "Writing CMake toolchain file..."
cat > "$SCRIPT_DIR/ios-linux-toolchain.cmake" << CMAKE
# iOS cross-compilation toolchain for Linux
# Auto-generated by tools/setup_linux_toolchain.sh

set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_SYSTEM_PROCESSOR arm64)

set(IOS_TOOLCHAIN_DIR "$TOOLCHAIN_DIR")
set(IOS_SDK_PATH "$SDK_PATH")

set(CMAKE_C_COMPILER   "$TOOLCHAIN_DIR/bin/ios-clang")
set(CMAKE_CXX_COMPILER "$TOOLCHAIN_DIR/bin/ios-clang++")

set(CMAKE_OSX_SYSROOT           "$SDK_PATH")
set(CMAKE_OSX_ARCHITECTURES     arm64)
set(CMAKE_OSX_DEPLOYMENT_TARGET 15.0)

set(CMAKE_FIND_ROOT_PATH "$SDK_PATH")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
CMAKE
success "CMake toolchain file → tools/ios-linux-toolchain.cmake"

# ── Done ───────────────────────────────────────────────────────
echo ""
echo -e "${GREEN}${BOLD}  ✓ Toolchain ready! No Mac required.${NC}"
echo ""
echo "  SDK:       $SDK_PATH"
echo "  Compiler:  $TOOLCHAIN_DIR/bin/ios-clang"
echo ""
echo "  Now run: ./build.sh"
echo ""
