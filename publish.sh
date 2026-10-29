#!/usr/bin/env bash
# Build Planula-ePaper firmware and stage it in the Coeno repo for OTA serving.
#
#   ./publish.sh                      # Coeno repo found as ../coeno
#   COENO_REPO=/path/to/coeno ./publish.sh
#
# Produces <coeno>/firmware/planula-epaper/planula-epaper-<build>.bin, where <build>
# is this repo's git commit count: the same number version.py bakes into
# FW_BUILD, so the manifest's build always matches the image (no update loop).
#
# Then commit + push in the Coeno repo. The Pi serves it after its next
# `git pull`/Update; nodes on an older build update within 6 h, or right away
# with `update` on the USB console.
set -euo pipefail
cd "$(dirname "$0")"

if [ -n "$(git status --porcelain)" ]; then
  echo "Uncommitted changes here. Commit first so the build number means something." >&2
  exit 1
fi

COENO="${COENO_REPO:-../coeno}"
if [ ! -d "${COENO}/firmware" ]; then
  echo "Coeno repo not found at ${COENO} (set COENO_REPO)." >&2
  exit 1
fi

BUILD=$(git rev-list --count HEAD)
echo "Building Planula-ePaper firmware (build ${BUILD}) for the e-paper board..."
pio run -e epaper

DEST="${COENO}/firmware/planula-epaper"
mkdir -p "${DEST}"
cp ".pio/build/epaper/firmware.bin" "${DEST}/planula-epaper-${BUILD}.bin"

echo
echo "Staged ${DEST}/planula-epaper-${BUILD}.bin"
echo "Next, in ${COENO}:"
echo "  git add firmware/planula-epaper/planula-epaper-${BUILD}.bin && git commit -m 'planula-epaper fw build ${BUILD}' && git push"
echo "Prune older planula-epaper-*.bin there to keep the repo lean (only the highest build is served)."
