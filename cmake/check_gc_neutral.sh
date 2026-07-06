#!/bin/sh
# Fail the build if a Mach-O carries __objc_imageinfo.
#
# 10.9 System Preferences runs Objective-C garbage collection and REFUSES to load any bundle that
# carries an __objc_imageinfo section — so our injected payload (the prefpane osax / SIMBL bundle /
# dylib) MUST be pure C: Cocoa via the objc runtime only (objc_msgSend / sel_registerName), never
# @interface/@implementation/@"...". A single stray Obj-C construct silently reintroduces the section
# and the payload stops loading. This check is wired as a POST_BUILD step on those targets so the
# build FAILS loudly instead of a developer having to remember to run `otool -l | grep imageinfo`.
set -eu
bin="$1"
if otool -l "$bin" 2>/dev/null | grep -qi imageinfo; then
  echo "" >&2
  echo "GC-NEUTRAL CHECK FAILED: $bin carries __objc_imageinfo." >&2
  echo "  10.9 System Preferences (ObjC GC) will REFUSE to load it." >&2
  echo "  You compiled Obj-C syntax into a GC-neutral payload — use the objc runtime only" >&2
  echo "  (objc_msgSend / sel_registerName / objc_getClass), no @interface/@implementation/@\"...\"." >&2
  echo "" >&2
  exit 1
fi
