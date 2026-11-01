#!/usr/bin/env bash
# The display a check's session runs at and the reference it compares with (the device-size plan, step 1), sourced
# by the size-relevant checks and tools/android-reference.sh. MINATO_DISPLAY=<width>x<height>@<scale> names the
# screen in pixels; without it, the M4 plan's: the iPhone 15 Pro Max's 1290 x 2796 px at scale 3. The sweep's
# displays are the iPhone Duo's (tools/display-sweep.sh): folded 1398x2034@3 and open 2007x2853@3.
# Sets CHECK_DISPLAY, CHECK_WIDTH, CHECK_HEIGHT, CHECK_SCALE and CHECK_DENSITY (scale x 160, Android's dpi) and
# defines expected_at_display.
CHECK_DEFAULT_DISPLAY=1290x2796@3
CHECK_DISPLAY="${MINATO_DISPLAY:-$CHECK_DEFAULT_DISPLAY}"
if [[ ! "$CHECK_DISPLAY" =~ ^([1-9][0-9]*)x([1-9][0-9]*)@([1-9])$ ]]; then
  echo "MINATO_DISPLAY is <width>x<height>@<scale> with a whole scale, as 1398x2034@3, not $CHECK_DISPLAY" >&2
  exit 2
fi
CHECK_WIDTH=${BASH_REMATCH[1]}
CHECK_HEIGHT=${BASH_REMATCH[2]}
CHECK_SCALE=${BASH_REMATCH[3]}
CHECK_DENSITY=$((CHECK_SCALE * 160))

# expected_at_display <name> <kind>: Android 15's reference fixtures/expected/<name>.<kind> at the M4 plan's display,
# fixtures/expected/<name>@<width>x<height>.<kind> at another, which tools/android-reference.sh makes with the same
# MINATO_DISPLAY. A reference that is the same at every display keeps its one name and is not asked through this.
expected_at_display() {
  if [[ "$CHECK_DISPLAY" == "$CHECK_DEFAULT_DISPLAY" ]]; then
    echo "fixtures/expected/$1.$2"
  else
    echo "fixtures/expected/$1@${CHECK_WIDTH}x${CHECK_HEIGHT}.$2"
  fi
}
