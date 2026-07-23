# kext_load_notice.sh -- sourceable helper for the pkg postinstall.
#
# The preinstall unloads any resident prior kext so the new one can load (IOKit class names are global;
# a same-generation build left resident would block the load). That unload is best-effort, and macOS
# Installer can't conditionally raise a restart dialog from a script -- so instead of silently shipping
# a load that couldn't happen, we OBSERVE the outcome and, only when warranted, tell the user to restart.
#
# maybe_notify_kext_stuck <kext_id> <message>: after the loader was triggered, wait briefly for the
# (async, WatchPaths-fired) load, then notify the console user to restart ONLY if the kext still isn't
# resident AND a GUI user is logged in. At the login window the wrapper's /dev/console guard legitimately
# defers the load to next login -- that is not a stuck update, so stay silent. Returns 0 if all is well
# (loaded, or correctly deferred), 1 if it notified. All externals are overridable for tests.
: "${KL_KEXTSTAT:=kextstat}"
: "${KL_STAT_CONSOLE:=stat -f%Su /dev/console}"
: "${KL_WAIT:=8}"          # seconds to wait for the async trigger-fired load before concluding it failed
: "${KL_NOTIFY:=}"         # override: called as `$KL_NOTIFY <user> <msg>`; default = osascript as that user

_kl_resident() { $KL_KEXTSTAT 2>/dev/null | grep -q "$1"; }

maybe_notify_kext_stuck() {
    _kl_id="$1"; _kl_msg="$2"; _kl_n="$KL_WAIT"
    while ! _kl_resident "$_kl_id"; do
        [ "$_kl_n" -le 0 ] && break
        _kl_n=$((_kl_n - 1)); sleep 1
    done
    _kl_resident "$_kl_id" && return 0                          # it loaded -> nothing to say

    _kl_user="$($KL_STAT_CONSOLE 2>/dev/null || echo root)"
    [ -n "$_kl_user" ] && [ "$_kl_user" != "root" ] || return 0 # login window: guard deferred by design

    # Logged in but the kext never came up -> a resident prior build likely blocked it; a reboot clears it.
    if [ -n "$KL_NOTIFY" ]; then
        "$KL_NOTIFY" "$_kl_user" "$_kl_msg"
    else
        # Backgrounded: `display dialog` is modal and would block the installer's postinstall until the
        # user clicked OK. Run it detached in the user's GUI session so the install completes regardless.
        sudo -u "$_kl_user" osascript -e \
            "display dialog \"$_kl_msg\" buttons {\"OK\"} default button 1 with icon note" >/dev/null 2>&1 &
    fi
    return 1
}
