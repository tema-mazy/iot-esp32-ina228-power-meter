#!/bin/sh
# Build a release: firmware image, notes, and the numbers an OTA needs.
#
#   ./release.sh              package HEAD as vYY.MM-<hash>
#   ./release.sh --dry        build and report, write nothing
#   ./release.sh v26.08-abc1234    re-package an existing version
#
# Version scheme is vYY.MM-<short hash>: the month says roughly when, the hash
# says exactly what. Because the hash is part of the version, the tag is
# derived from HEAD rather than supplied, and there is no way to tag the wrong
# commit. release.sh creates the tag if it does not exist; delete it with
# `git tag -d <version>` if you change your mind.
#
# Produces dist/power-meter-<version>/ containing:
#   firmware.bin       the image to feed to ATFW
#   release_notes.md   this month's CHANGELOG section plus the commit list
#   MD5SUMS            md5 of firmware.bin - ATFW needs exactly this value
#   manifest.txt       size, md5, chip, IDF version, source commit
#   integration.md     host API spec, copied so the tarball stands alone
# and the tarball dist/power-meter-<version>.tar.gz
set -e

die() { echo "release: $*" >&2; exit 1; }

cd "$(dirname "$0")"
ROOT="$(pwd)"

DRY=""
VERSION=""
for arg in "$@"; do
  case "$arg" in
    --dry) DRY=1 ;;
    -*)    die "unknown option $arg" ;;
    *)     VERSION="$arg" ;;
  esac
done

# -- preconditions ------------------------------------------------------------

[ -n "$IDF_PATH" ] || die "IDF_PATH unset - source \$HOME/esp/<ver>/export.sh first"
command -v idf.py >/dev/null 2>&1 || die "idf.py not on PATH - source export.sh first"
git rev-parse --git-dir >/dev/null 2>&1 || die "not a git repository"

# A dirty tree is refused rather than warned about. ESP-IDF derives PROJECT_VER
# from `git describe`, so an uncommitted build reports "<sha>-dirty" over ATI -
# and two different dirty builds are indistinguishable, which turns any
# rollback or fleet-inventory decision into guesswork.
[ -z "$(git status --porcelain)" ] || die "working tree is dirty. Commit or stash first,
     or the image reports its version as '<sha>-dirty' and two different
     builds become indistinguishable over ATI."

HEAD_SHA="$(git rev-parse --short HEAD)"

if [ -n "$VERSION" ]; then
  git rev-parse "$VERSION" >/dev/null 2>&1 || die "version '$VERSION' is not a tag"
  TAG_SHA="$(git rev-parse --short "$VERSION^{commit}")"
  [ "$HEAD_SHA" = "$TAG_SHA" ] \
    || die "HEAD ($HEAD_SHA) is not at $VERSION ($TAG_SHA); check out the tag first"
else
  VERSION="v$(date '+%y.%m')-$HEAD_SHA"
  if git rev-parse "$VERSION" >/dev/null 2>&1; then
    echo "release: reusing existing tag $VERSION"
  elif [ -n "$DRY" ]; then
    echo "release: would create tag $VERSION"
  else
    git tag -a "$VERSION" -m "release $VERSION"
    echo "release: created tag $VERSION"
  fi
fi

MONTH="${VERSION%%-*}"      # v26.08, the CHANGELOG section key

# -- build --------------------------------------------------------------------

echo "release: building $VERSION"
idf.py build >/dev/null || die "build failed - run 'idf.py build' to see why"

BIN="$ROOT/build/power_meter.bin"
[ -f "$BIN" ] || die "build produced no $BIN"

SIZE="$(wc -c < "$BIN" | tr -d ' ')"
if command -v md5 >/dev/null 2>&1; then
  MD5="$(md5 -q "$BIN")"              # BSD / macOS
else
  MD5="$(md5sum "$BIN" | cut -d' ' -f1)"
fi

# Confirm the image really carries the version. If PROJECT_VER did not pick up
# the tag, every consumer of ATI is misled and the release is worse than
# useless. A tag created after the last build needs a rebuild to take effect,
# which is why this checks the artefact rather than trusting the tag.
if command -v strings >/dev/null 2>&1; then
  strings "$BIN" | grep -qx "$VERSION" || {
    echo "release: '$VERSION' absent from the image, rebuilding to pick up the tag" >&2
    idf.py fullclean >/dev/null 2>&1 || true
    idf.py build >/dev/null || die "rebuild failed"
    SIZE="$(wc -c < "$BIN" | tr -d ' ')"
    if command -v md5 >/dev/null 2>&1; then MD5="$(md5 -q "$BIN")"
    else MD5="$(md5sum "$BIN" | cut -d' ' -f1)"; fi
    strings "$BIN" | grep -qx "$VERSION" \
      || die "image still does not report $VERSION - check PROJECT_VER"
  }
fi

echo "release: firmware.bin  $SIZE bytes  md5 $MD5"

if [ -n "$DRY" ]; then
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

  # This month's section: from "## [vYY.MM]" to the next "## [". Keyed by month
  # rather than full version because the hash is not knowable until the commit
  # exists, so a section can never be written for it in advance.
  SECTION=""
  if [ -f "$ROOT/CHANGELOG.md" ]; then
    SECTION="$(awk -v key="$MONTH" '
      $0 ~ "^## \\[" key "\\]" { inside = 1; next }
      inside && /^## \[/       { exit }
      inside                   { print }
    ' "$ROOT/CHANGELOG.md")"
  fi
  if [ -n "$(printf '%s' "$SECTION" | tr -d '[:space:]')" ]; then
    printf '%s\n' "$SECTION"
  else
    echo "_No \`## [$MONTH]\` section in CHANGELOG.md - see the commit list below._"
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
echo "  push the tag   :  git push origin $VERSION"
