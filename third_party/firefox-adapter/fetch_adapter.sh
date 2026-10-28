#!/bin/bash
# Fetch the latest quarterly stable release
# This script is run during build - only updates if a new quarterly release is available

REPO="spivanatalie64/acreetion-surf-webkit-to-firefox-addon-adapter"
ADAPTER_DIR="$(dirname "$0")"
CURRENT_VERSION=$(cat "$ADAPTER_DIR/VERSION" 2>/dev/null || echo "none")

# Get latest release tag
LATEST_TAG=$(curl -s "https://api.github.com/repos/$REPO/releases/latest" 2>/dev/null | grep '"tag_name":' | sed -E 's/.*"([^"]+)".*/\1/')

if [ -z "$LATEST_TAG" ] || [ "$LATEST_TAG" = "null" ]; then
    echo "No release found, using current version: $CURRENT_VERSION"
    exit 0
fi

if [ "$LATEST_TAG" = "$CURRENT_VERSION" ]; then
    echo "Adapter is up to date: $CURRENT_VERSION"
    exit 0
fi

echo "New quarterly release available: $LATEST_TAG (current: $CURRENT_VERSION)"
echo "Fetching release artifacts..."

# Download release
curl -sL "https://github.com/$REPO/releases/download/$LATEST_TAG/acreetion-surf-webkit-to-firefox-addon-adapter-*.tar.gz" -o /tmp/adapter.tar.gz 2>&1 | tail -2

if [ -f /tmp/adapter.tar.gz ]; then
    tar xzf /tmp/adapter.tar.gz -C "$ADAPTER_DIR" --strip-components=1
    echo "$LATEST_TAG" > "$ADAPTER_DIR/VERSION"
    echo "Updated to $LATEST_TAG"
else
    echo "Failed to download release, keeping current version: $CURRENT_VERSION"
fi
