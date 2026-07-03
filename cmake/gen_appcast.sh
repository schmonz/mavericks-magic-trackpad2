#!/bin/sh
# Generate the Sparkle appcast.xml for ONE release, to stdout.
#
#   gen_appcast.sh <version> <pkg-url> <min-os> <notes-file> <enclosure-attrs>
#
# Release notes are inlined verbatim from <notes-file> (docs/release-notes/vX.Y.Z.md) into the
# <description> CDATA. <enclosure-attrs> is the `sparkle:edSignature="..." length="..."` string that
# `sign_update -s <key> <pkg>` prints -- passed in, so THIS script is pure text and unit-testable
# without the signing key (sign_update is a modern Swift binary that can't run on the 10.9 box).
#
# Fails if the notes file is missing or empty: the notes ARE the release's <description>, and a
# release with no notes is a mistake we want to catch (mirrors the CI + pre-push gate).
set -eu

[ $# -eq 5 ] || { echo "usage: gen_appcast.sh <version> <pkg-url> <min-os> <notes-file> <enclosure-attrs>" >&2; exit 2; }
VER="$1"; URL="$2"; MINOS="$3"; NOTES_FILE="$4"; ENCLOSURE_ATTRS="$5"

[ -f "$NOTES_FILE" ] || { echo "release notes not found: $NOTES_FILE" >&2; exit 1; }
[ -s "$NOTES_FILE" ] || { echo "release notes empty: $NOTES_FILE" >&2; exit 1; }

# RFC-822 pubDate in UTC (Sparkle sorts items by it).
PUBDATE=$(date -u "+%a, %d %b %Y %H:%M:%S +0000")

cat <<XML
<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel>
    <title>Mavericks Trackpad 2</title>
    <item>
      <title>Version $VER</title>
      <pubDate>$PUBDATE</pubDate>
      <sparkle:version>$VER</sparkle:version>
      <sparkle:shortVersionString>$VER</sparkle:shortVersionString>
      <sparkle:minimumSystemVersion>$MINOS</sparkle:minimumSystemVersion>
      <description><![CDATA[
$(cat "$NOTES_FILE")
]]></description>
      <enclosure url="$URL" type="application/octet-stream" $ENCLOSURE_ATTRS />
    </item>
  </channel>
</rss>
XML
