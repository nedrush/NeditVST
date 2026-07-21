#!/usr/bin/env bash
set -euo pipefail

echo "=== Installing build dependencies ==="
sudo apt update
sudo apt install -y \
    cmake \
    libasound2-dev \
    libjack-dev \
    ladspa-sdk \
    libfreetype-dev \
    libcurl4-openssl-dev \
    libgl-dev \
    libx11-dev \
    libxcomposite-dev \
    libxcursor-dev \
    libxext-dev \
    libxinerama-dev \
    libxrandr-dev \
    libxrender-dev \
    libwebkit2gtk-4.1-dev

echo "=== Configuring ==="
cmake -B build -DCMAKE_BUILD_TYPE=Release

echo "=== Building ==="
cmake --build build --config Release -j"$(nproc)"

echo "=== Done ==="
ls -la build/NeditVST_artefacts/Release/VST3/ 2>/dev/null || echo "Check build/ for output"
