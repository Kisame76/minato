#!/usr/bin/env bash
# Prints -allowProvisioningUpdates when a build should ask Apple for a new profile, and nothing otherwise: xcodebuild
# talks to Apple (and needs the Xcode account to be signed in) only with that flag, so the builds pass it only when
# the local profile for one of the bundle IDs is missing or expires within 36 hours. A profile that is still valid
# builds without any account; the user signs in about once a week.
# Usage: PROFILES=$(tools/provisioning-updates.sh <bundle id>...)   (unquoted, so empty gives no argument)
# Reads "$HOME/Library/Developer/Xcode/UserData/Provisioning Profiles", or PROFILE_DIR. Each profile holds a plain XML
# plist inside its CMS envelope, which is cut out between "<?xml" and "</plist>" (no signature check: Xcode does it).
# A profile covers a bundle ID by its application-identifier without the team prefix, wildcards included. The newest
# profile of a bundle ID counts. The team and the account are never printed.
set -euo pipefail
[[ $# -gt 0 ]] || { echo "usage: provisioning-updates.sh <bundle id>..." >&2; exit 2; }
exec python3 - "$@" <<'PY'
import datetime
import fnmatch
import glob
import os
import plistlib
import sys

MARGIN = datetime.timedelta(hours=36)
folder = os.environ.get("PROFILE_DIR") or os.path.join(
    os.path.expanduser("~"), "Library/Developer/Xcode/UserData/Provisioning Profiles")
now = datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)

profiles = []  # (bundle id pattern, expiry)
for path in glob.glob(os.path.join(folder, "*")):
    try:
        data = open(path, "rb").read()
        start, end = data.index(b"<?xml"), data.index(b"</plist>") + len(b"</plist>")
        info = plistlib.loads(data[start:end])
        identifier = info["Entitlements"]["application-identifier"]
        profiles.append((identifier.split(".", 1)[1], info["ExpirationDate"].replace(tzinfo=None)))
    except (OSError, ValueError, KeyError, IndexError, plistlib.InvalidFileException):
        continue  # not a profile this tool reads: it counts as none

ask = False
for bundle in sys.argv[1:]:
    expiries = [expiry for pattern, expiry in profiles if fnmatch.fnmatchcase(bundle, pattern)]
    newest = max(expiries) if expiries else None
    if newest is not None and newest - now > MARGIN:
        continue
    ask = True
    if newest is None:
        print("note  there is no profile for %s; asking Apple for a new one (needs the Xcode account)" % bundle,
              file=sys.stderr)
    else:
        print("note  the profile for %s expires %s; asking Apple for a new one (needs the Xcode account)"
              % (bundle, newest.strftime("%Y-%m-%d %H:%M UTC")), file=sys.stderr)
if ask:
    print("-allowProvisioningUpdates")
PY
