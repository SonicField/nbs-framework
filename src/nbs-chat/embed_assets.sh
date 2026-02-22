#!/bin/bash
# embed_assets.sh — Generate web_assets.h from HTML source file
#
# Converts web_assets/index.html into a C static const char array.
# The HTML file contains inline CSS and JS, so only one asset is needed.
#
# Output: C header with static const char asset_index_html[]
#
# Usage: bash embed_assets.sh > web_assets.h

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
HTML_FILE="$SCRIPT_DIR/web_assets/index.html"

if [ ! -f "$HTML_FILE" ]; then
    echo "Error: $HTML_FILE not found" >&2
    exit 1
fi

cat << 'HEADER'
/*
 * web_assets.h — Auto-generated embedded web assets
 *
 * DO NOT EDIT — regenerate with: bash embed_assets.sh > web_assets.h
 */

#ifndef NBS_WEB_ASSETS_H
#define NBS_WEB_ASSETS_H

static const char asset_index_html[] =
HEADER

# Convert file to C string literal lines
# Escape: backslash, double-quote, and format as "line\n"
sed 's/\\/\\\\/g; s/"/\\"/g' "$HTML_FILE" | \
    awk '{ printf "    \"%s\\n\"\n", $0 }'

echo ";"
echo ""
echo "#endif /* NBS_WEB_ASSETS_H */"
