#!/usr/bin/env bash
# Checks the session's view, app/Sources/Session/SessionWindow.swift, on the Mac with UIKit under Mac Catalyst
# (ADR-0043 c4 (d)): app/HostChecks/main.swift asks which view takes the guest's touches while graphics/host's hide
# rule hides the window's layer, and where the SurfaceViews' layers lie. It builds the APKSession files the view uses
# as their module, with a stand-in for LaunchRequest.swift's LaunchMode, and the view with one for graphics/host's
# MinatoMetalLayer, in Swift 6 as the app. The view is laid out at MINATO_DISPLAY, by default the M4 plan's
# (tools/check-display.sh).
set -euo pipefail
cd "$(dirname "$0")/.."
source tools/check-display.sh
OUT=build/session-window-check
mkdir -p "$OUT"
SDK=$(xcrun --sdk macosx --show-sdk-path)
FLAGS=(-target arm64-apple-ios26.0-macabi -sdk "$SDK" -Fsystem "$SDK/System/iOSSupport/System/Library/Frameworks"
       -swift-version 6)
SESSION=importer/APKKit/Sources/APKSession
xcrun --sdk macosx swiftc "${FLAGS[@]}" -parse-as-library -emit-library -emit-module -module-name APKSession \
  -emit-module-path "$OUT/APKSession.swiftmodule" -o "$OUT/libAPKSession.dylib" \
  "$SESSION/SessionBridge.swift" "$SESSION/OpenRequest.swift" "$SESSION/Lifecycle.swift" \
  "$SESSION/SurfaceRequest.swift" "$SESSION/TouchTranslator.swift" "$SESSION/EdgePanReport.swift" \
  "$SESSION/MenuSwipe.swift" \
  "$SESSION/SessionOrientation.swift" "$SESSION/InterfaceOrientations.swift" "$SESSION/SoftKeyboard.swift" \
  "$SESSION/HardwareKeyboard.swift" "$SESSION/KeyboardCover.swift" "$SESSION/TextBar.swift" \
  "$SESSION/OutsideTap.swift" "$SESSION/GuestInsets.swift" "$SESSION/WindowRequest.swift" \
  "$SESSION/WindowOrder.swift" "$SESSION/WindowTree.swift" app/HostChecks/LaunchMode.swift
xcrun --sdk macosx swiftc "${FLAGS[@]}" -I "$OUT" -L "$OUT" -lAPKSession -Xlinker -rpath -Xlinker "$PWD/$OUT" \
  -o "$OUT/session-window-check" app/Sources/Session/SessionWindow.swift app/Sources/Session/KeyboardBarView.swift \
  app/HostChecks/MinatoMetalLayer.swift \
  app/HostChecks/main.swift
MINATO_DISPLAY="$CHECK_DISPLAY" "$OUT/session-window-check"
