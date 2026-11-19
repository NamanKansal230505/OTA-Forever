#!/usr/bin/env bash
# Uploads dist/ to the GitHub release "ota-<channel>". Devices read
#   https://github.com/<repo>/releases/download/ota-<channel>/manifest.json
# Needs: GH_TOKEN, GH_REPO, TAG, BUILD, GITHUB_SHA
set -euo pipefail
cd dist

if ! gh release view "$TAG" >/dev/null 2>&1; then
  gh release create "$TAG" --target "$GITHUB_SHA" --latest=false \
    --title "OTA channel: ${TAG#ota-}" \
    --notes "Managed by OTA-Forever. Devices on this branch install from here."
fi

# Binaries first, manifest last: a device never sees a manifest pointing at a missing file.
gh release upload "$TAG" firmware-*-b"${BUILD}".bin --clobber
gh release upload "$TAG" manifest.json --clobber
gh release edit "$TAG" --notes "Build #${BUILD} from commit ${GITHUB_SHA:0:7}. Managed by OTA-Forever; devices on this branch install from here."

# Remove binaries of older builds
gh release view "$TAG" --json assets --jq '.assets[].name' | while read -r name; do
  case "$name" in
    firmware-*-b"${BUILD}".bin | manifest.json) ;;
    firmware-*.bin) gh release delete-asset "$TAG" "$name" -y ;;
  esac
done

echo "Published build #${BUILD} to release ${TAG}"
