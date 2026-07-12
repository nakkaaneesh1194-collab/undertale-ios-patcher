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
SDK_URL="https://github.com/xybp888/iOS-SDKs/releases/download/iOS${SDK_VERSION}-SDKs/iPhoneOS${SDK_VERSION}.sdk.zip"

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

info()    { echo -e "  ${CYAN}→${NC} $1"; }
success() { echo -e "  ${GREEN}✓${NC} $1"; }
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
if [ ! -f "$SDK_TARBALL" ]; then
    info "Downloading iPhoneOS $SDK_VERSION SDK..."
    TMP_ZIP=$(mktemp /tmp/iPhoneOS.sdk.XXXXXX.zip)
    curl -fsSL --progress-bar -L "$SDK_URL" -o "$TMP_ZIP" \
        || error "Failed to download SDK. Check your internet connection."
    info "Extracting SDK..."
    TMP_EXTRACT=$(mktemp -d /tmp/iPhoneOS.sdk.XXXXXX)
    unzip -q "$TMP_ZIP" -d "$TMP_EXTRACT/"
    rm -f "$TMP_ZIP"
    # Find the actual .sdk folder (ignore __MACOSX)
    EXTRACTED_SDK=$(find "$TMP_EXTRACT" -maxdepth 2 -name "iPhoneOS*.sdk" -type d | head -1)
    [ -n "$EXTRACTED_SDK" ] || error "SDK folder not found after extraction"
    mkdir -p "$TOOLCHAIN_DIR/sdk"
    # Re-pack as .tar.gz so cctools-port can consume it
    info "Repacking SDK as tarball for cctools-port..."
    tar -czf "$SDK_TARBALL" -C "$(dirname "$EXTRACTED_SDK")" "$(basename "$EXTRACTED_SDK")"
    rm -rf "$TMP_EXTRACT"
    success "SDK ready: $SDK_NAME"
else
    success "SDK already present: $SDK_NAME"
fi
SDK_PATH="$TOOLCHAIN_DIR/sdk/$SDK_NAME"
# Also extract for direct use by the compiler wrappers
if [ ! -d "$SDK_PATH" ]; then
    tar -xzf "$SDK_TARBALL" -C "$TOOLCHAIN_DIR/sdk/"
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
