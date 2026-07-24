#!/bin/sh
# migrate_autocheck.sh OLD_DOMAIN NEW_DOMAIN
#
# Carry the Sparkle "check automatically" opt-in (SUEnableAutomaticChecks) across an updater IDENTITY
# rename. The setting lives in the updater's per-user preferences domain, so renaming the updater
# (com.schmonz.* -> dev.modernmavericks.*) hands it a fresh, empty domain -- and a user who had enabled
# automatic checks would silently revert to the built-in (off) default. This copies the old value forward.
#
# Runs AS THE TARGET USER (the postinstall invokes it via `sudo -u <console user>`) because CFPreferences
# domains are per-user; run as root it would read root's prefs, not the person's.
#
# Idempotent + non-destructive: migrate ONLY when the OLD domain set an explicit value AND the NEW domain
# has none yet -- so it never clobbers a choice made under the new identity, and a second run is a no-op.
# Best-effort: touches only these two domains, and never returns nonzero for a "nothing to do" case.
set -u
OLD_DOM=${1:-}
NEW_DOM=${2:-}
KEY=SUEnableAutomaticChecks
[ -n "$OLD_DOM" ] && [ -n "$NEW_DOM" ] || { echo "usage: $0 OLD_DOMAIN NEW_DOMAIN" >&2; exit 2; }

# Already chosen under the new identity? Leave it -- an explicit new-domain value wins.
if defaults read "$NEW_DOM" "$KEY" >/dev/null 2>&1; then
    exit 0
fi
# Nothing set under the old identity? Nothing to carry -- leave the new default in place.
OLDVAL=$(defaults read "$OLD_DOM" "$KEY" 2>/dev/null) || exit 0
[ -n "$OLDVAL" ] || exit 0
# `defaults read` of a boolean yields 1/0; normalise any truthy/falsey spelling to a real bool write.
case "$OLDVAL" in
    1|TRUE|true|YES|yes) defaults write "$NEW_DOM" "$KEY" -bool true ;;
    0|FALSE|false|NO|no)  defaults write "$NEW_DOM" "$KEY" -bool false ;;
    *) exit 0 ;;
esac
exit 0
