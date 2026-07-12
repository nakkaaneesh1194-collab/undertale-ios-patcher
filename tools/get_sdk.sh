#!/usr/bin/env bash
# get_sdk.sh — run this ONCE on any Mac with Xcode installed.
# Packages the iPhoneOS SDK into a tarball you can copy to your Linux machine.
#
# Usage (on Mac):
#   bash get_sdk.sh
#   # Outputs: iPhoneOS.sdk.tar.gz in the current directory
#   # Copy that file to your Linux machine's undertale-ios-patcher/tools/ folder

set -e

SDK_PATH=$(xcrun --sdk iphoneos --show-sdk-path 2>/dev/null)
if [ -z "$SDK_PATH" ]; then
    echo "Error: Xcode not found. Install Xcode and run: sudo xcode-select --switch /Applications/Xcode.app"
    exit 1
fi

SDK_NAME=$(basename "$SDK_PATH")
echo "Found SDK: $SDK_PATH"
echo "Packaging $SDK_NAME ..."

tar -czf "iPhoneOS.sdk.tar.gz" -C "$(dirname "$SDK_PATH")" "$SDK_NAME"

echo ""
echo "Done! Copy iPhoneOS.sdk.tar.gz to your Linux machine's"
echo "undertale-ios-patcher/tools/ directory, then run:"
echo "  bash tools/setup_linux_toolchain.sh"
