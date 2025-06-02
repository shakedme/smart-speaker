#!/bin/bash

# Script to create precompiled component libraries
# This creates .a files for ESP-IDF/ESP-ADF components that rarely change

set -e

echo "🏭 Creating precompiled component libraries..."

# Create libs directory
mkdir -p precompiled_libs

# Build the project first to generate all component libraries
echo "📦 Building project to generate component libraries..."
idf.py build

# Copy ESP-IDF component libraries that rarely change
echo "📚 Copying ESP-IDF component libraries..."
cp build/esp-idf/*/lib*.a precompiled_libs/ 2>/dev/null || true

# Copy ESP-ADF component libraries
echo "📚 Copying ESP-ADF component libraries..."
cp build/esp-idf/esp-adf-libs/libesp-adf-libs.a precompiled_libs/ 2>/dev/null || true
cp build/esp-idf/*/libaudio_*.a precompiled_libs/ 2>/dev/null || true
cp build/esp-idf/*/libbluetooth_*.a precompiled_libs/ 2>/dev/null || true

# List what we've got
echo "✅ Precompiled libraries created:"
ls -lh precompiled_libs/ | grep "\.a$" | wc -l | xargs echo "  Total libraries:"
ls -lh precompiled_libs/ | tail -10

echo "💡 Note: These libraries are specific to your current ESP-IDF/ESP-ADF version and configuration."
echo "💡 Regenerate them when updating ESP-IDF/ESP-ADF or changing major config options." 