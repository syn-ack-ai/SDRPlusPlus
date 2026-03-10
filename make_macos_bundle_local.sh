#!/bin/sh
set -e

# ========================= Configuration =========================
BUILD_DIR="build"
BUNDLE="dist/SDR++.app"
NUM_CORES=$(sysctl -n hw.ncpu)

# CMake options for modules that need extra dependencies
# Enable modules for which you have the libraries installed
CMAKE_EXTRA_OPTS=""

# Detect available Homebrew libraries and enable matching modules
if [ -d "/opt/homebrew/lib" ] || [ -d "/usr/local/lib" ]; then
    # BladeRF
    if [ -f "/opt/homebrew/lib/libbladeRF.dylib" ] || [ -f "/usr/local/lib/libbladeRF.dylib" ]; then
        CMAKE_EXTRA_OPTS="$CMAKE_EXTRA_OPTS -DOPT_BUILD_BLADERF_SOURCE=ON"
    fi
    # LimeSDR
    if [ -f "/opt/homebrew/lib/libLimeSuite.dylib" ] || [ -f "/usr/local/lib/libLimeSuite.dylib" ]; then
        CMAKE_EXTRA_OPTS="$CMAKE_EXTRA_OPTS -DOPT_BUILD_LIMESDR_SOURCE=ON"
    fi
    # SoapySDR
    if [ -f "/opt/homebrew/lib/libSoapySDR.dylib" ] || [ -f "/usr/local/lib/libSoapySDR.dylib" ]; then
        CMAKE_EXTRA_OPTS="$CMAKE_EXTRA_OPTS -DOPT_BUILD_SOAPY_SOURCE=ON"
    fi
    # FobosSDR
    if [ -f "/opt/homebrew/lib/libfobos.dylib" ] || [ -f "/usr/local/lib/libfobos.dylib" ]; then
        CMAKE_EXTRA_OPTS="$CMAKE_EXTRA_OPTS -DOPT_BUILD_FOBOSSDR_SOURCE=ON"
    fi
    # RFNM
    if [ -f "/opt/homebrew/lib/librfnm.dylib" ] || [ -f "/usr/local/lib/librfnm.dylib" ]; then
        CMAKE_EXTRA_OPTS="$CMAKE_EXTRA_OPTS -DOPT_BUILD_RFNM_SOURCE=ON"
    fi
    # Codec2 (for M17 decoder)
    if [ -f "/opt/homebrew/lib/libcodec2.dylib" ] || [ -f "/usr/local/lib/libcodec2.dylib" ]; then
        CMAKE_EXTRA_OPTS="$CMAKE_EXTRA_OPTS -DOPT_BUILD_M17_DECODER=ON"
    fi
    # PortAudio (new sink)
    if [ -f "/opt/homebrew/lib/libportaudio.dylib" ] || [ -f "/usr/local/lib/libportaudio.dylib" ]; then
        CMAKE_EXTRA_OPTS="$CMAKE_EXTRA_OPTS -DOPT_BUILD_NEW_PORTAUDIO_SINK=ON"
    fi
fi

echo "========================================"
echo "  SDR++ macOS Local Build Script"
echo "========================================"
echo ""

# ========================= Step 1: CMake Configure =========================
echo "[1/5] Configuring with CMake..."
mkdir -p $BUILD_DIR
cd $BUILD_DIR
cmake .. \
    -DUSE_BUNDLE_DEFAULTS=ON \
    $CMAKE_EXTRA_OPTS
cd ..
echo ""

# ========================= Step 2: Build =========================
echo "[2/5] Building with $NUM_CORES cores..."
cmake --build $BUILD_DIR -j $NUM_CORES
echo ""

# ========================= Step 3: Create Bundle =========================
echo "[3/5] Creating macOS app bundle..."

source macos/bundle_utils.sh

# Clear .app
rm -rf $BUNDLE
mkdir -p dist

# Create .app structure
bundle_create_struct $BUNDLE

# Add resources
cp -R root/res/* $BUNDLE/Contents/Resources/

# Create the icon file
bundle_create_icns root/res/icons/sdrpp.macos.png $BUNDLE/Contents/Resources/sdrpp

# Create the property list
bundle_create_plist sdrpp SDR++ org.sdrpp.sdrpp 1.2.1 sdrp sdrpp sdrpp $BUNDLE/Contents/Info.plist

# Core
bundle_install_binary $BUNDLE $BUNDLE/Contents/MacOS $BUILD_DIR/sdrpp
bundle_install_binary $BUNDLE $BUNDLE/Contents/Frameworks $BUILD_DIR/core/libsdrpp_core.dylib

# Source modules
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/source_modules/airspy_source/airspy_source.dylib
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/source_modules/airspyhf_source/airspyhf_source.dylib
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/source_modules/bladerf_source/bladerf_source.dylib
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/source_modules/file_source/file_source.dylib
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/source_modules/fobossdr_source/fobossdr_source.dylib
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/source_modules/hackrf_source/hackrf_source.dylib
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/source_modules/hermes_source/hermes_source.dylib
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/source_modules/limesdr_source/limesdr_source.dylib
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/source_modules/network_source/network_source.dylib
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/source_modules/plutosdr_source/plutosdr_source.dylib
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/source_modules/rfnm_source/rfnm_source.dylib
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/source_modules/rfspace_source/rfspace_source.dylib
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/source_modules/rtl_sdr_source/rtl_sdr_source.dylib
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/source_modules/rtl_tcp_source/rtl_tcp_source.dylib
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/source_modules/sdrpp_server_source/sdrpp_server_source.dylib
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/source_modules/spectran_http_source/spectran_http_source.dylib
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/source_modules/spyserver_source/spyserver_source.dylib

# Sink modules
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/sink_modules/portaudio_sink/audio_sink.dylib
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/sink_modules/network_sink/network_sink.dylib
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/sink_modules/new_portaudio_sink/new_portaudio_sink.dylib

# Decoder modules
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/decoder_modules/atv_decoder/atv_decoder.dylib
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/decoder_modules/m17_decoder/m17_decoder.dylib
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/decoder_modules/meteor_demodulator/meteor_demodulator.dylib
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/decoder_modules/pager_decoder/pager_decoder.dylib
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/decoder_modules/radio/radio.dylib

# Misc modules
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/misc_modules/discord_integration/discord_integration.dylib
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/misc_modules/frequency_manager/frequency_manager.dylib
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/misc_modules/iq_exporter/iq_exporter.dylib
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/misc_modules/recorder/recorder.dylib
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/misc_modules/rigctl_client/rigctl_client.dylib
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/misc_modules/rigctl_server/rigctl_server.dylib
bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/misc_modules/scanner/scanner.dylib
echo ""

# ========================= Step 4: Fix permissions =========================
echo "[4/5] Fixing library permissions..."

# Fix permissions on all framework dylibs (Homebrew copies may be read-only)
chmod 755 "$BUNDLE/Contents/Frameworks/"*.dylib 2>/dev/null || true
chmod 755 "$BUNDLE/Contents/Plugins/"*.dylib 2>/dev/null || true
chmod 755 "$BUNDLE/Contents/MacOS/"* 2>/dev/null || true
echo ""

# ========================= Step 5: Sign =========================
echo "[5/5] Signing app bundle..."

# Remove extended attributes (FinderInfo, provenance, etc.) that block codesign
xattr -cr "$BUNDLE" 2>/dev/null || true
find "$BUNDLE" -exec xattr -c {} \; 2>/dev/null || true

# Sign
codesign --force --deep --sign - "$BUNDLE"

echo ""
echo "========================================"
echo "  Build complete!"
echo "  App bundle: $BUNDLE"
echo "========================================"
echo ""
echo "To run: open $BUNDLE"
