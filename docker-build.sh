#!/bin/bash
set -euo pipefail
cd /work

STATUS_FILE="/work/build_status.json"
rm -f "$STATUS_FILE"

BUILD_RC=1
TEST_RC=-1
ARCH_INFO=""
NEEDED_INFO=""

finish() {
  local rc=$?
  if [ "$BUILD_RC" -eq 0 ] && [ "$rc" -ne 0 ]; then
    BUILD_RC=$rc
  fi
  cat > "$STATUS_FILE" <<EOF
{
  "exit_code": $BUILD_RC,
  "test_ts_rc": $TEST_RC,
  "arch": "$(printf '%s' "$ARCH_INFO" | tr '"' "'")",
  "needed": "$(printf '%s' "$NEEDED_INFO" | tr '\n' ' ' | tr '"' "'")"
}
EOF
}
trap finish EXIT

CROSS_COMPILE="${CROSS_COMPILE:-$(ls /opt/*/bin/*-gcc 2>/dev/null | head -n1 | sed 's/gcc$//')}"
[ -n "$CROSS_COMPILE" ] || { echo "ERROR: No aarch64 cross compiler found in /opt"; exit 1; }
CC="${CROSS_COMPILE}gcc"
READELF="${CROSS_COMPILE}readelf"
SYSROOT="$($CC -print-sysroot)"
echo "Using CC=$CC"
echo "Using SYSROOT=$SYSROOT"

EXTRA_CFLAGS=""
LDFLAGS=""
if [ ! -f "$SYSROOT/usr/include/SDL2/SDL_ttf.h" ]; then
  SDL_HDR_DIR="$(dirname "$(find /opt "$SYSROOT" -name SDL_ttf.h 2>/dev/null | head -n1)")"
  if [ -n "$SDL_HDR_DIR" ] && [ "$SDL_HDR_DIR" != "." ]; then
    EXTRA_CFLAGS="-I$SDL_HDR_DIR"
    echo "Detected extra SDL include: $SDL_HDR_DIR"
  fi
fi

make clean
make CROSS_COMPILE="$CROSS_COMPILE" SYSROOT="$SYSROOT" EXTRA_CFLAGS="$EXTRA_CFLAGS" LDFLAGS="$LDFLAGS" tailscale_ui
BUILD_RC=0

echo "=== Binary inspection ==="
ARCH_INFO="$(file tailscale_ui 2>/dev/null || true)"
echo "file tailscale_ui: $ARCH_INFO"
NEEDED_INFO="$($READELF -d tailscale_ui 2>/dev/null | grep NEEDED || true)"
echo "$NEEDED_INFO"
echo "=== Max GLIBC symbol versions ==="
$READELF -V tailscale_ui 2>/dev/null | grep -E 'GLIBC_[0-9]+\.[0-9]+' | sort -u || true

echo "=== Packaging dist/Tailscale ==="
rm -rf /work/dist/Tailscale
mkdir -p /work/dist/Tailscale/bin /work/dist/Tailscale/scripts /work/dist/Tailscale/certs
cp /work/tailscale_ui /work/dist/Tailscale/tailscale_ui
cp /work/pkg/launch.sh /work/dist/Tailscale/launch.sh
cp /work/pkg/config.json /work/dist/Tailscale/config.json
if [ -f /work/pkg/config.ini ]; then
  cp /work/pkg/config.ini /work/dist/Tailscale/config.ini
else
  cp /work/pkg/config.ini.example /work/dist/Tailscale/config.ini
fi
cp /work/pkg/config.ini.example /work/dist/Tailscale/config.ini.example
cp /work/pkg/buttons.ini /work/dist/Tailscale/buttons.ini
cp /work/pkg/icon.png /work/dist/Tailscale/icon.png
cp /work/pkg/scripts/daemon.sh /work/dist/Tailscale/scripts/daemon.sh
cp /work/pkg/certs/cacert.pem /work/dist/Tailscale/certs/cacert.pem
cp /work/pkg/bin/tailscale /work/dist/Tailscale/bin/tailscale
cp /work/pkg/bin/tailscaled /work/dist/Tailscale/bin/tailscaled
chmod +x /work/dist/Tailscale/tailscale_ui /work/dist/Tailscale/launch.sh \
         /work/dist/Tailscale/scripts/daemon.sh /work/dist/Tailscale/bin/tailscale /work/dist/Tailscale/bin/tailscaled

echo "file tailscale: $(file /work/dist/Tailscale/bin/tailscale 2>/dev/null || true)"
echo "file tailscaled: $(file /work/dist/Tailscale/bin/tailscaled 2>/dev/null || true)"

if command -v gcc >/dev/null 2>&1; then
  echo "=== Building & running host test_ts ==="
  make HOST_CC=gcc test_ts
  set +e
  ./test_ts
  TEST_RC=$?
  set -e
  echo "test_ts exited with code $TEST_RC"
else
  echo "Host gcc not installed in container; skipping host test_ts"
fi

if command -v zip >/dev/null 2>&1; then
  (cd /work/dist && rm -f Tailscale.zip && zip -qr Tailscale.zip Tailscale)
  echo "Created /work/dist/Tailscale.zip"
fi

echo "=== Build finished successfully ==="
