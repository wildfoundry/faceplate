#!/usr/bin/env bash
# Build Faceplate (kmscon) for linux/aarch64 in Docker — fast CM5 iterate path.
# Matches dataplicity-os faceplate_git.bb DRM2D / libseat / freetype settings.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="${ROOT}/build-docker-aarch64"
IMAGE="${FACEPLATE_BUILD_IMAGE:-ubuntu:24.04}"
PLATFORM="${FACEPLATE_BUILD_PLATFORM:-linux/arm64}"

mkdir -p "${OUT_DIR}"

echo "Building Faceplate in ${IMAGE} (${PLATFORM})"
echo "Output dir: ${OUT_DIR}"

/usr/local/bin/docker run --rm \
  --platform "${PLATFORM}" \
  -v "${ROOT}:/src:ro" \
  -v "${OUT_DIR}:/out" \
  -w /work \
  "${IMAGE}" \
  bash -lc '
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq \
  meson ninja-build pkg-config gcc git ca-certificates \
  zlib1g-dev libpng-dev \
  libudev-dev libxkbcommon-dev libdrm-dev \
  libfreetype6-dev libfontconfig1-dev \
  libseat-dev libsystemd-dev \
  >/tmp/apt.log 2>&1 || { tail -50 /tmp/apt.log; exit 1; }

# Copy tree so wrap-git can fetch libtsm (source mount is read-only)
rm -rf /work/src /out/build /out/kmscon /out/faceplate
mkdir -p /work
cp -a /src /work/src
cd /work/src

meson setup /out/build \
  --prefix=/usr \
  --libdir=lib \
  -Dwerror=false \
  -Ddocs=disabled \
  -Dtests=false \
  -Ddbus=disabled \
  -Dextra_debug=false \
  -Dlibseat=enabled \
  -Dvideo_fbdev=enabled \
  -Dvideo_drm2d=enabled \
  -Dvideo_drm3d=disabled \
  -Drenderer_gltex=disabled \
  -Dfont_freetype=enabled \
  -Dfont_pango=disabled \
  -Dfont_psf=enabled \
  -Dfont_unifont=enabled \
  -Dlibtsm:tests=false \
  -Dlibtsm:extra_debug=false

meson compile -C /out/build
# Meson places the executable under build/src/
BIN="$(find /out/build -type f -name kmscon -perm -111 | head -1)"
test -n "$BIN" && test -x "$BIN"
install -m0755 "$BIN" /out/kmscon
cp -a /out/kmscon /out/faceplate
COLLECTOR="$(find /out/build -type f -name faceplate-collector -perm -111 | head -1 || true)"
if [ -n "$COLLECTOR" ]; then
  install -m0755 "$COLLECTOR" /out/faceplate-collector
fi

# Deploy bundle matching device paths (/usr/bin + /usr/lib/kmscon)
rm -rf /out/deploy
mkdir -p /out/deploy/usr/bin /out/deploy/usr/lib/kmscon
install -m0755 /out/faceplate /out/deploy/usr/bin/faceplate
if [ -n "${COLLECTOR:-}" ]; then
  install -m0755 "$COLLECTOR" /out/deploy/usr/bin/faceplate-collector
fi
find /out/build -path '*/font/mod-*.so' -exec install -m0755 {} /out/deploy/usr/lib/kmscon/ \;

ls -la /out/faceplate
ldd /out/faceplate || true
echo "OK: /out/faceplate"
echo "Deploy bundle: /out/deploy"
' | tee "${OUT_DIR}/build-log.txt"

echo
echo "Binary: ${OUT_DIR}/faceplate"
ls -la "${OUT_DIR}/faceplate" "${OUT_DIR}/kmscon"
