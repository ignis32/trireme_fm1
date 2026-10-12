#!/bin/sh
# Every .noinit object in our sources must be volatile (and not static): nothing initialises that RAM, and the
# compiler may otherwise assume a static one starts at zero (softbrick_cause.MD).
bad=$(grep -rn 'section("\.noinit")' "$@" | grep -v volatile)
[ -z "$bad" ] && exit 0
echo "check_noinit: .noinit objects must be volatile:" >&2
echo "$bad" >&2
exit 1
