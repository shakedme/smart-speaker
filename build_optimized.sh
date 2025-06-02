#!/bin/bash

# Optimized ESP32 Build Script
# This script maximizes build speed by using ccache, incremental builds, and precompiled libraries

set -e  # Exit on any error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}🚀 Starting Optimized ESP32 Build${NC}"

# Source ESP-IDF environment if needed
if ! command -v idf.py &> /dev/null; then
    echo -e "${YELLOW}⚙️  Setting up ESP-IDF environment...${NC}"
    if [ -f "$HOME/esp/esp-idf/export.sh" ]; then
        source "$HOME/esp/esp-idf/export.sh"
    elif [ -f "/opt/esp-idf/export.sh" ]; then
        source "/opt/esp-idf/export.sh"
    elif [ -f "$HOME/Projects/esp-adf/esp-idf/export.sh" ]; then
        source "$HOME/Projects/esp-adf/esp-idf/export.sh"
    else
        echo -e "${RED}❌ ESP-IDF not found. Please run: source ~/Projects/esp-adf/esp-idf/export.sh${NC}"
        exit 1
    fi
fi

# Enable ccache for this session
export IDF_CCACHE_ENABLE=1

# Check if ccache is available
if ! command -v ccache &> /dev/null; then
    echo -e "${YELLOW}⚠️  ccache not found. Install with: brew install ccache${NC}"
else
    echo -e "${GREEN}✅ ccache enabled${NC}"
    ccache -s | grep "Cache size" || true
fi

# Parse arguments
BUILD_TYPE=${1:-release}
USE_PRECOMPILED=${2:-auto}  # auto, yes, no, clean
FLASH_OPTION=${3:-}

# Check for precompiled libraries
PRECOMPILED_AVAILABLE=false
if [ -d "precompiled_libs" ] && [ "$(ls -A precompiled_libs 2>/dev/null)" ]; then
    PRECOMPILED_AVAILABLE=true
fi

# Determine whether to use precompiled libs
USE_PRECOMPILED_LIBS=false
if [[ "$USE_PRECOMPILED" == "yes" ]] || ([[ "$USE_PRECOMPILED" == "auto" ]] && [[ "$PRECOMPILED_AVAILABLE" == true ]]); then
    USE_PRECOMPILED_LIBS=true
fi

echo -e "${BLUE}📋 Build Configuration:${NC}"
echo "  - Build Type: $BUILD_TYPE"
echo "  - ccache: $(if [[ $IDF_CCACHE_ENABLE == 1 ]]; then echo 'Enabled'; else echo 'Disabled'; fi)"
echo "  - Incremental: Yes (preserving build directory)"
echo "  - Precompiled Libs: $(if [[ "$USE_PRECOMPILED_LIBS" == true ]]; then echo 'Enabled'; else echo 'Disabled'; fi)"
if [[ "$PRECOMPILED_AVAILABLE" == true ]]; then
    echo "  - Available Precompiled: $(ls precompiled_libs/*.a 2>/dev/null | wc -l | xargs) libraries"
fi

# Handle clean builds
if [[ "$USE_PRECOMPILED" == "clean" ]]; then
    echo -e "${YELLOW}🧹 Cleaning build directory...${NC}"
    idf.py fullclean
    USE_PRECOMPILED_LIBS=false
fi

# Setup precompiled libraries if using them
if [[ "$USE_PRECOMPILED_LIBS" == true ]]; then
    echo -e "${BLUE}📚 Setting up precompiled libraries...${NC}"
    # Create symlinks to precompiled libraries in build directory
    mkdir -p build/esp-idf
    for lib in precompiled_libs/*.a; do
        if [ -f "$lib" ]; then
            lib_name=$(basename "$lib")
            component_name=$(echo "$lib_name" | sed 's/^lib//' | sed 's/\.a$//')
            
            # Create component directory and link library
            mkdir -p "build/esp-idf/$component_name"
            ln -sf "../../../$lib" "build/esp-idf/$component_name/$lib_name" 2>/dev/null || true
        fi
    done
    echo -e "${GREEN}✅ Precompiled libraries linked${NC}"
fi

# Build with optimizations
echo -e "${BLUE}🔨 Building project...${NC}"
start_time=$(date +%s)

# Set build flags for precompiled libraries
if [[ "$USE_PRECOMPILED_LIBS" == true ]]; then
    export CMAKE_ARGS="-DPRECOMPILED_LIBS=ON"
fi

if [[ "$BUILD_TYPE" == "debug" ]]; then
    # Debug build - slower but with debug symbols
    idf.py build
else
    # Release build - optimized for speed and size
    idf.py -DCMAKE_BUILD_TYPE=Release build
fi

end_time=$(date +%s)
build_duration=$((end_time - start_time))

echo -e "${GREEN}✅ Build completed in ${build_duration}s${NC}"

# Show ccache stats if available
if command -v ccache &> /dev/null; then
    echo -e "${BLUE}📊 ccache statistics:${NC}"
    ccache -s | grep -E "(Hits|Misses|Cache size)" | sed 's/^/  /'
fi

# Optional: Flash if requested
if [[ "$FLASH_OPTION" == "flash" ]]; then
    echo -e "${BLUE}📲 Flashing to device...${NC}"
    idf.py flash
fi

echo -e "${GREEN}🎉 Done!${NC}"
echo ""
echo -e "${BLUE}💡 Usage examples:${NC}"
echo "  ./build_optimized.sh                    # Auto-detect precompiled libs"
echo "  ./build_optimized.sh release yes        # Force use precompiled libs"
echo "  ./build_optimized.sh release no         # Skip precompiled libs"
echo "  ./build_optimized.sh release clean      # Clean build (ignore precompiled)"
echo "  ./build_optimized.sh debug auto flash   # Debug build + flash" 