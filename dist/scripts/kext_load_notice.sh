# kext_load_notice.sh -- sourceable helper for the pkg postinstall.
#
# A running kext driving a live device cannot be hot-swapped in place: its IOKit classes (our terminal
# publishes a synthetic IOHIDDevice the system HID stack retains -- exactly as upstream VoodooInput's
# VoodooInputSimulatorDevice does) keep instances live, so kextunload can't unload it and a same-id new
# kext can't load over it. So on an UPDATE we don't hot-swap: we leave the old kext driving the trackpad
# and let the new one load cleanly at the next boot (nothing resident then). This helper tells the user.
#
# maybe_notify_user <message>: show <message> to the logged-in GUI user; stay silent at the login window
# (console owned by root -- no one to tell; the load will happen at their next login/boot anyway). The
# console-user lookup and the notifier are overridable for tests.
: "${KL_STAT_CONSOLE:=stat -f%Su /dev/console}"
: "${KL_NOTIFY:=}"    # override: called as `$KL_NOTIFY <user> <msg>`; default = osascript as that user

maybe_notify_user() {
    _kl_msg="$1"
    _kl_user="$($KL_STAT_CONSOLE 2>/dev/null || echo root)"
    [ -n "$_kl_user" ] && [ "$_kl_user" != "root" ] || return 0   # login window: no one to notify
    if [ -n "$KL_NOTIFY" ]; then
        "$KL_NOTIFY" "$_kl_user" "$_kl_msg"
    else
        # Backgrounded: `display dialog` is modal and would otherwise block the installer's postinstall
        # until the user clicked OK. Run it detached in the user's GUI session.
        sudo -u "$_kl_user" osascript -e \
            "display dialog \"$_kl_msg\" buttons {\"OK\"} default button 1 with icon note" >/dev/null 2>&1 &
    fi
}
