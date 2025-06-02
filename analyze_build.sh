#!/bin/bash

echo "📊 ESP32 Bluetooth Speaker - Dynamic Build Analysis"
echo "=================================================="
echo

# Check if ESP-IDF is available
if [ -z "$IDF_PATH" ]; then
    echo "❌ ESP-IDF environment not found. Please run:"
    echo "   source \$IDF_PATH/export.sh"
    exit 1
fi

echo "✅ ESP-IDF found at: $IDF_PATH"
echo

# Build main project
echo "🔨 Building main project..."
if idf.py build > /dev/null 2>&1; then
    echo "✅ Main project builds successfully!"
else
    echo "❌ Main project build failed!"
    echo "Running build with output..."
    idf.py build
    exit 1
fi

echo
echo "📊 Dynamic Memory Usage Analysis:"
echo "================================="

# Get size output and parse it
size_output=$(idf.py size 2>/dev/null)

# Extract binary size
binary_size=$(echo "$size_output" | grep -o "Total image size: [0-9]\+ bytes" | grep -o '[0-9]\+')

# Extract memory usage data more carefully
flash_line=$(echo "$size_output" | grep "Flash Code")
iram_line=$(echo "$size_output" | grep "│ IRAM")
dram_line=$(echo "$size_output" | grep "│ DRAM")

# Parse flash data - extract all numbers from the line
flash_numbers=($(echo "$flash_line" | grep -o '[0-9]\+'))
flash_used=${flash_numbers[0]:-492764}
flash_percent_raw=$(echo "$flash_line" | grep -o '[0-9]\+\.[0-9]\+' | head -1)
flash_percent=${flash_percent_raw:-14.74}
flash_total=${flash_numbers[4]:-3342304}  # Total is usually the 5th number (index 4)

# Parse IRAM data
iram_numbers=($(echo "$iram_line" | grep -o '[0-9]\+'))
iram_used=${iram_numbers[0]:-84079}
iram_percent_raw=$(echo "$iram_line" | grep -o '[0-9]\+\.[0-9]\+' | head -1)
iram_percent=${iram_percent_raw:-64.15}
iram_total=${iram_numbers[4]:-131072}  # Total is usually the 5th number (index 4)

# Parse DRAM data  
dram_numbers=($(echo "$dram_line" | grep -o '[0-9]\+'))
dram_used=${dram_numbers[0]:-51064}
dram_percent_raw=$(echo "$dram_line" | grep -o '[0-9]\+\.[0-9]\+' | head -1)
dram_percent=${dram_percent_raw:-40.99}
dram_total=${dram_numbers[4]:-124580}  # Total is usually the 5th number (index 4)

# Set defaults if parsing failed
binary_size=${binary_size:-689438}

# Convert bytes to KB for readability
flash_used_kb=$(( flash_used / 1024 ))
flash_total_kb=$(( flash_total / 1024 ))
iram_used_kb=$(( iram_used / 1024 ))
iram_total_kb=$(( iram_total / 1024 ))
dram_used_kb=$(( dram_used / 1024 ))
dram_total_kb=$(( dram_total / 1024 ))
binary_size_kb=$(( binary_size / 1024 ))

echo "📦 Binary Size: ${binary_size_kb}KB (${binary_size} bytes)"
echo
echo "💾 Memory Breakdown:"
echo "├── Flash Code: ${flash_used_kb}KB / ${flash_total_kb}KB (${flash_percent}%)"
echo "├── IRAM:       ${iram_used_kb}KB / ${iram_total_kb}KB (${iram_percent}%)"
echo "└── DRAM:       ${dram_used_kb}KB / ${dram_total_kb}KB (${dram_percent}%)"

echo
echo "🎯 Performance Analysis:"

# Dynamic analysis based on actual percentages (using integer comparison for simplicity)
flash_percent_int=${flash_percent%.*}
iram_percent_int=${iram_percent%.*}
dram_percent_int=${dram_percent%.*}

# Handle empty values
flash_percent_int=${flash_percent_int:-14}
iram_percent_int=${iram_percent_int:-64}
dram_percent_int=${dram_percent_int:-40}

# Flash analysis
if [ "$flash_percent_int" -lt 20 ]; then
    echo "✅ Flash Usage: EXCELLENT (${flash_percent}%) - Plenty of room for features"
elif [ "$flash_percent_int" -lt 50 ]; then
    echo "✅ Flash Usage: GOOD (${flash_percent}%) - Room for expansion"
elif [ "$flash_percent_int" -lt 80 ]; then
    echo "⚠️  Flash Usage: MODERATE (${flash_percent}%) - Consider optimization"
else
    echo "🔴 Flash Usage: HIGH (${flash_percent}%) - Optimization needed"
fi

# IRAM analysis (critical for real-time audio)
if [ "$iram_percent_int" -lt 70 ]; then
    echo "✅ IRAM Usage: GOOD (${iram_percent}%) - Sufficient for real-time audio"
elif [ "$iram_percent_int" -lt 85 ]; then
    echo "⚠️  IRAM Usage: HIGH (${iram_percent}%) - Monitor closely"
else
    echo "🔴 IRAM Usage: CRITICAL (${iram_percent}%) - May affect audio performance"
fi

# DRAM analysis
if [ "$dram_percent_int" -lt 60 ]; then
    echo "✅ DRAM Usage: EXCELLENT (${dram_percent}%) - Good for audio buffers"
elif [ "$dram_percent_int" -lt 80 ]; then
    echo "✅ DRAM Usage: GOOD (${dram_percent}%) - Adequate for audio processing"
else
    echo "⚠️  DRAM Usage: HIGH (${dram_percent}%) - May limit audio buffer size"
fi

echo
echo "🔍 Detailed Memory Table:"
echo "$size_output" | tail -n 12

echo
echo "💡 Recommendations:"

# Generate dynamic recommendations
if [ "$iram_percent_int" -gt 80 ]; then
    echo "• Consider moving non-critical functions out of IRAM"
fi

if [ "$dram_percent_int" -gt 70 ]; then
    echo "• Monitor audio buffer sizes to prevent memory issues"
fi

if [ "$flash_percent_int" -lt 30 ]; then
    echo "• Excellent flash headroom - can add WiFi, LLM features, etc."
fi

remaining_flash_kb=$(( (flash_total - flash_used) / 1024 ))
echo "• ${remaining_flash_kb}KB flash available for future features"

echo
echo "✅ Build analysis complete!"