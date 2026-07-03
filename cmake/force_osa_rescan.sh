#!/bin/sh
# Force OpenScripting to re-scan /Library/ScriptingAdditions so a just-(re)installed handler is
# registered BEFORE the next System Preferences launch. Without it the OSA additions cache is stale
# after a bundle change, our MT2x/load event falls to System Prefs' default handler, and the osax
# doesn't load until a *later* launch (the "needs 2-3 relaunches" race). Best-effort: a hiccup here
# must never fail the install.
#
# Lives in a script file (not an inline CMake `COMMAND sh -c "…"`) on purpose: CMake's Unix Makefiles
# generator space-escapes a single quoted COMMAND string, which shreds the `>/dev/null 2>&1 || true`
# redirection into e.g. `/dev/null 2: Permission denied` + `true: command not found` (Error 127) and
# aborts the whole `make` chain — the exact bug the sibling comment in tools/CMakeLists.txt warns of.
osascript -e 'return 1' >/dev/null 2>&1 || true
