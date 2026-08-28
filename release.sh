#!/bin/sh
# Build a release: firmware image, notes, and the numbers an OTA needs.
#
#   ./release.sh              package HEAD as vYY.MM-<hash>
#   ./release.sh --dry        build and report, write nothing
#   ./release.sh --publish    package, push the tag, create a GitHub release
#   ./release.sh v26.08-abc1234    re-package an existing version
#
# Version scheme is vYY.MM-<short hash>: the month says roughly when, the hash
# says exactly what. Because the hash is part of the version, the tag is
# derived from HEAD rather than supplied, and there is no way to tag the wrong
# commit. release.sh creates the tag if it does not exist; delete it with
# `git tag -d <version>` if you change your mind.
#
# Produces two files in dist/:
#   power-meter-<version>.bin   the image to feed to ATFW
#   power-meter-<version>.md    notes: size, md5, the exact ATFW line, this
#                               month's CHANGELOG section, and the commits
#
# Deliberately just those two. The size and md5 an OTA needs are in the notes,
# so a separate MD5SUMS or manifest would only be a second copy to fall out of
# step, and integration.md lives in the repo where it is edited.
set -e

die() { echo "release: $*" >&2; exit 1; }

cd "$(dirname "$0")"
ROOT="$(pwd)"

DRY=""
PUBLISH=""
VERSION=""
for arg in "$@"; do
  case "$arg" in
    --dry)     DRY=1 ;;
    --publish) PUBLISH=1 ;;
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

echo "release: image  $SIZE bytes  md5 $MD5"

if [ -n "$DRY" ]; then
  echo "release: --dry, stopping before writing dist/"
  exit 0
fi

# -- notes --------------------------------------------------------------------

OUT="$ROOT/dist"
mkdir -p "$OUT"
NOTES="$OUT/power-meter-$VERSION.md"

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
  echo "| image | power-meter-$VERSION.bin |"
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
} > "$NOTES"

# -- package ------------------------------------------------------------------

cp "$BIN" "$OUT/power-meter-$VERSION.bin"

echo "release: wrote dist/power-meter-$VERSION.bin"
echo "release: wrote dist/power-meter-$VERSION.md"
echo
echo "  flash over USB :  idf.py -p <port> flash"
echo "  update over AT :  ATFW=$SIZE,$MD5"

# -- publish ------------------------------------------------------------------

if [ -z "$PUBLISH" ]; then
  echo
  echo "  publish        :  ./release.sh --publish   (needs gh, pushes the tag)"
  exit 0
fi

# Publishing needs no extra tooling: a GitHub release is two REST calls, and
# curl plus a token does both. gh would only be convenience, and installing a
# whole CLI to POST twice is not a trade worth making.
#
# Note that pushing the tag alone already produces a release page with
# auto-generated source archives. The API calls exist to attach the firmware
# image, which is the part that actually matters here.
[ -n "$GITHUB_TOKEN" ] || die "--publish needs GITHUB_TOKEN in the environment.

     There is no 'Releases' permission to look for - GitHub puts releases
     under Contents, which covers 'repository contents, commits, branches,
     downloads, releases, and merges'. So:

       fine-grained token : Repository permissions -> Contents: Read and write
       classic token      : the 'repo' scope (public_repo if the repo is public)

     Then:  export GITHUB_TOKEN=...
     Keep it out of any file git tracks."

REMOTE="$(git remote get-url origin)"
# git@github.com:owner/repo.git  or  https://github.com/owner/repo.git
SLUG="$(printf '%s' "$REMOTE" | sed -e 's#^.*github\.com[:/]##' -e 's#\.git$##')"
case "$SLUG" in
  */*) ;;
  *)   die "cannot parse owner/repo from origin: $REMOTE" ;;
esac

echo
echo "release: pushing tag $VERSION"
git push origin "$VERSION"

echo "release: creating release $SLUG $VERSION"
UPLOAD="$(python3 - "$SLUG" "$VERSION" "$NOTES" <<'PY'
import json, os, sys, urllib.request, urllib.error
slug, tag, notes_path = sys.argv[1], sys.argv[2], sys.argv[3]
body = json.dumps({
    "tag_name": tag,
    "name": "power-meter " + tag,
    "body": open(notes_path).read(),
}).encode()
req = urllib.request.Request(
    "https://api.github.com/repos/%s/releases" % slug, data=body,
    headers={"Authorization": "Bearer " + os.environ["GITHUB_TOKEN"],
             "Accept": "application/vnd.github+json",
             "Content-Type": "application/json",
             "User-Agent": "release.sh"})
try:
    r = json.load(urllib.request.urlopen(req))
except urllib.error.HTTPError as e:
    sys.exit("github: %s %s" % (e.code, e.read().decode()[:300]))
# upload_url arrives as a URI template ending {?name,label}
print(r["upload_url"].split("{")[0])
PY
)" || die "could not create the release"

ASSET="$OUT/power-meter-$VERSION.bin"
echo "release: uploading $(basename "$ASSET")"
curl -sS -f -X POST \
  -H "Authorization: Bearer $GITHUB_TOKEN" \
  -H "Content-Type: application/octet-stream" \
  --data-binary "@$ASSET" \
  "$UPLOAD?name=$(basename "$ASSET")" >/dev/null \
  || die "release created but the asset upload failed - attach it by hand, or
     delete the release and re-run"

echo "release: published https://github.com/$SLUG/releases/tag/$VERSION"
