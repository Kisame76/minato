#!/usr/bin/env bash
# Generate APKRunner.xcodeproj from project.yml.
# The development team comes from $DEVELOPMENT_TEAM or from the accounts in
# Xcode (Settings > Accounts), Personal Team first.
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ -z "${DEVELOPMENT_TEAM:-}" ]]; then
  DEVELOPMENT_TEAM="$(defaults export com.apple.dt.Xcode - 2>/dev/null | python3 -c '
import plistlib, sys

try:
    prefs = plistlib.loads(sys.stdin.buffer.read())
except Exception:
    sys.exit(0)

teams = []

def walk(node):
    if isinstance(node, dict):
        if "teamID" in node:
            teams.append((not node.get("isFreeProvisioningTeam", False), node["teamID"]))
        for value in node.values():
            walk(value)
    elif isinstance(node, list):
        for value in node:
            walk(value)

walk(prefs.get("IDEProvisioningTeamByIdentifier", {}))
print(sorted(teams)[0][1] if teams else "")
' || true)"
fi
export DEVELOPMENT_TEAM

xcodegen generate --quiet
echo "Generated APKRunner.xcodeproj (team: ${DEVELOPMENT_TEAM:-none})"
