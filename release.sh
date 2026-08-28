#!/bin/sh
# Build a release: firmware image, notes, and the numbers an OTA needs.
#
#   ./release.sh v1.2.0        package the tag v1.2.0
#   ./release.sh v1.2.0 --dry  build and report, write nothing to dist/
#
# Produces dist/power-meter-<version>/ containing:
#   firmware.bin       the image to feed to ATFW
#   release_notes.md   this version's CHANGELOG section plus the commit list
#   MD5SUMS            md5 of firmware.bin - ATFW needs exactly this value
#   manifest.txt       size, md5, chip, IDF version, source commit
#   integration.md     host API spec, copied so the tarball stands alone
# and the tarball dist/power-meter-<version>.tar.gz
#
# The version comes from a git tag, because that is what the firmware reports
# over ATI: ESP-IDF derives PROJECT_VER from `git describe`, so an untagged or
# dirty tree ships an image that identifies itself as a bare SHA or as
# "<sha>-dirty". A host cannot tell two dirty builds apart, which makes OTA
# rollback decisions guesswork. Hence the clean-tree and tag checks below are
# refusals, not warnings.
set -e

VERSION="$1"
DRY="$2"

die() { echo "release: $*" >&2; exit 1; }

[ -n "$VERSION" ] || die "usage: ./release.sh <version-tag> [--dry]"

cd "$(dirname "$0")"
ROOT="$(pwd)"

# -- preconditions ------------------------------------------------------------

[ -n "$IDF_PATH" ] || die "IDF_PATH unset - source \$HOME/esp/<ver>/export.sh first"
command -v idf.py >/dev/null 2>&1 || die "idf.py not on PATH - source export.sh first"

git rev-parse --git-dir >/dev/null 2>&1 || die "not a git repository"

if [ -n "$(git status --porcelain)" ]; then
  die "working tree is dirty. Commit or stash first, or the image will report
     its version as '<sha>-dirty' and two different builds become
     indistinguishable over ATI."
fi

git rev-parse "$VERSION" >/dev/null 2>&1 || die "tag '$VERSION' does not exist.
     Create it first:  git tag -a $VERSION -m 'release $VERSION'"

HEAD_SHA="$(git rev-parse --short HEAD)"
TAG_SHA="$(git rev-parse --short "$VERSION^{commit}")"
[ "$HEAD_SHA" = "$TAG_SHA" ] || die "HEAD ($HEAD_SHA) is not at tag $VERSION ($TAG_SHA).
     Check out the tag before releasing."

# -- build --------------------------------------------------------------------

echo "release: building $VERSION at $HEAD_SHA"
idf.py build >/dev/null || die "build failed - run 'idf.py build' to see why"

BIN="$ROOT/build/power_meter.bin"
[ -f "$BIN" ] || die "build produced no $BIN"

SIZE="$(wc -c < "$BIN" | tr -d ' ')"
if command -v md5 >/dev/null 2>&1; then
  MD5="$(md5 -q "$BIN")"              # BSD / macOS
else
  MD5="$(md5sum "$BIN" | cut -d' ' -f1)"
fi

# Confirm the image really carries the tag. If PROJECT_VER did not pick it up,
# every consumer of ATI is misled and the release is worse than useless.
if command -v strings >/dev/null 2>&1; then
  strings "$BIN" | grep -qx "$VERSION" \
    || echo "release: WARNING - '$VERSION' not found in the image; check that" \
            "PROJECT_VER picked up the tag (idf.py fullclean may be needed)" >&2
fi

echo "release: firmware.bin  $SIZE bytes  md5 $MD5"

if [ "$DRY" = "--dry" ]; then
  echo "release: --dry, stopping before writing dist/"
  exit 0
fi

# -- notes --------------------------------------------------------------------

OUT="$ROOT/dist/power-meter-$VERSION"
rm -rf "$OUT"
mkdir -p "$OUT"

PREV="$(git describe --tags --abbrev=0 "$VERSION^" 2>/dev/null || true)"
if [ -n "$PREV" ]; then
  RANGE="$PREV..$VERSION"
  SINCE="Changes since $PREV"
else
  RANGE="$VERSION"
  SINCE="All commits (first tagged release)"
fi

{
  echo "# power-meter $VERSION"
  echo
  echo "Built $(date -u '+%Y-%m-%d %H:%M UTC') from $HEAD_SHA."
  echo
  echo "| | |"
  echo "|---|---|"
  echo "| image | firmware.bin |"
  echo "| size | $SIZE bytes |"
  echo "| md5 | \`$MD5\` |"
  echo "| target | esp32c3 |"
  echo
  echo "## Install over the AT link"
  echo
  echo '```'
  echo "ATFW=$SIZE,$MD5"
  echo '```'
  echo
  echo "The device replies \`OK <chunk>\`, then expects the raw image in chunks"
  echo "of that many bytes, acknowledging each with a single \`.\` character."
  echo "See integration.md S5. Do not hardcode the chunk size - it is announced"
  echo "per build precisely so it can change."
  echo
  echo "## Changelog"
  echo

  # Pull this version's section out of CHANGELOG.md: from the "## [VERSION]"
  # heading to the next "## [" heading.
  if [ -f "$ROOT/CHANGELOG.md" ]; then
    awk -v ver="$VERSION" '
      $0 ~ "^## \\[" ver "\\]" { inside = 1; next }
      inside && /^## \[/       { exit }
      inside                   { print }
    ' "$ROOT/CHANGELOG.md" | sed '/^$/{ x; /./d; x; }'
  fi

  echo
  echo "## $SINCE"
  echo
  git log --no-merges --pretty='- %s (%h)' "$RANGE"
} > "$OUT/release_notes.md"

# -- package ------------------------------------------------------------------

cp "$BIN" "$OUT/firmware.bin"
[ -f "$ROOT/integration.md" ] && cp "$ROOT/integration.md" "$OUT/"
echo "$MD5  firmware.bin" > "$OUT/MD5SUMS"

{
  echo "version=$VERSION"
  echo "commit=$HEAD_SHA"
  echo "size=$SIZE"
  echo "md5=$MD5"
  echo "target=esp32c3"
  echo "idf=$(idf.py --version 2>/dev/null | tail -1)"
  echo "built=$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
} > "$OUT/manifest.txt"

tar -czf "$ROOT/dist/power-meter-$VERSION.tar.gz" -C "$ROOT/dist" "power-meter-$VERSION"

echo "release: wrote dist/power-meter-$VERSION.tar.gz"
echo
echo "  flash over USB :  idf.py -p <port> flash"
echo "  update over AT :  ATFW=$SIZE,$MD5"
