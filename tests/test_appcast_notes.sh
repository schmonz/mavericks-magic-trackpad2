#!/bin/sh
# Asserts the shared gen_appcast.sh (mavericks-shared-cmake) renders our Markdown release notes to the
# expected HTML fragment (Sparkle shows <description> in a WebView; raw Markdown collapses to one blob).
set -e
here=$(dirname "$0")
root=$(cd "$here/.." && pwd)
# gen_appcast.sh now lives in mavericks-shared-cmake (located via the find_package user registry).
_msc=$(cat "$HOME/.cmake/packages/MavericksSharedCMake/"* 2>/dev/null | head -1)
gen="$_msc/scripts/gen_appcast.sh"
[ -f "$gen" ] || { echo "SKIP: mavericks-shared-cmake not installed ($gen)"; exit 0; }

tmp="${TMPDIR:-/tmp}/mt2-appcast-notes.$$"
mkdir -p "$tmp"
trap 'rm -rf "$tmp"' EXIT

# --- synthetic fixture exercising every construct ---------------------------------------------------
cat > "$tmp/notes.md" <<'MD'
## Title X

Lead para with **bold** and *italic* words.

- a
- item **two** spanning
  a continuation line
- three

Requires macOS 10.9.5 or later.
MD

out=$(sh "$gen" --render-notes "$tmp/notes.md")

check() { echo "$out" | grep -qF "$1" || { echo "FAIL: missing [$1]"; echo "--- rendered ---"; echo "$out"; exit 1; }; }

check '<h2>Title X</h2>'
check '<p>Lead para with <strong>bold</strong> and <em>italic</em> words.</p>'
check '<ul>'
check '<li>a</li>'
check '<li>item <strong>two</strong> spanning a continuation line</li>'   # continuation folds in
check '<li>three</li>'
check '</ul>'
check '<p>Requires macOS 10.9.5 or later.</p>'

# raw Markdown markers must NOT leak into the HTML
if echo "$out" | grep -qE '(\*\*|^- |^## )'; then
  echo "FAIL: raw Markdown leaked into rendered HTML"; echo "$out"; exit 1
fi

# --- real shipped notes: heading + a bullet must both render -----------------------------------------
real=$(sh "$gen" --render-notes "$root/docs/release-notes/v0.3.0.md")
echo "$real" | grep -qF '<h2>Mavericks Trackpad 2 — v0.3.0</h2>' || { echo "FAIL: v0.3.0 heading"; echo "$real"; exit 1; }
echo "$real" | grep -qF '<ul>' || { echo "FAIL: v0.3.0 has no list"; echo "$real"; exit 1; }
echo "$real" | grep -qF '<strong>Check for Updates, in the pane</strong>' || { echo "FAIL: v0.3.0 bold bullet lead"; echo "$real"; exit 1; }

# full appcast path also embeds the rendered HTML (not raw Markdown) inside the CDATA
full=$(sh "$gen" "Mavericks Trackpad 2" 0.3.0 https://example/pkg 10.9.5 "$root/docs/release-notes/v0.3.0.md" 'sparkle:edSignature="x" length="1"')
echo "$full" | grep -qF '<h2>Mavericks Trackpad 2' || { echo "FAIL: appcast lacks rendered heading"; echo "$full"; exit 1; }

echo OK
