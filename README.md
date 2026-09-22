# APK-Runner (Arbeitstitel)

Android-Apps und -Spiele (APKs) nativ auf dem iPhone ausführen, ohne Emulator. Privates Forschungsprojekt, verteilt nur per Sideload.

- Architektur: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)
- Entscheidungen (ADRs): [`docs/DECISIONS.md`](docs/DECISIONS.md)
- Stand und nächste Schritte: [`docs/STATUS.md`](docs/STATUS.md)

## Bauen

Voraussetzungen: Xcode 27, XcodeGen, CMake und Ninja (`brew install xcodegen cmake ninja`), Android NDK r30 unter `~/Library/Android/sdk/ndk/30.0.16248370` (anderer Ort: `ANDROID_NDK` setzen).

```bash
tools/sim.sh                 # im Simulator bauen und starten
tools/deploy.sh              # auf dem angeschlossenen iPhone bauen, installieren und starten
tools/gen.sh                 # nur das Xcode-Projekt erzeugen, z. B. vor dem Öffnen in Xcode
tools/build-native.sh --app  # C/C++-Runtime für die App bauen (build/APKRuntime.xcframework), vor dem ersten Build in Xcode
```

`APKRunner.xcodeproj`, `app/Info.plist` und `app/APKRunner.entitlements` werden aus [`project.yml`](project.yml) erzeugt und nicht versioniert. Änderungen daher in `project.yml` vornehmen, nicht in Xcode.

## Tests und Werkzeuge

```bash
swift test --package-path importer/APKKit                      # Unit-Tests (Parser und Library)
swift run --package-path importer/APKKit apk-inspect <apk>     # zeigt, was die App aus einer APK liest
tools/apk-check.py <apk>                                        # Echtheit prüfen, bevor eine APK benutzt wird
tools/build-native.sh                                           # C/C++: NDK-Fixtures, Tests auf dem Mac (CTest), XCFramework
build/native/macos/importer/converter/apk-convert inspect <so>  # zeigt, was der Loader in einer ELF-Datei sieht
```

Der C/C++-Teil (`runtime/`, `importer/converter/`) baut mit CMake-Presets ([`CMakePresets.json`](CMakePresets.json)), die NDK-Testprogramme in `fixtures/` baut `tools/build-fixtures.sh`.

Test-APKs liegen lokal in `apk/` (von Git ignoriert), siehe [`docs/TEST-APKS.md`](docs/TEST-APKS.md).

## Lizenz

Apache-2.0, siehe [`LICENSE`](LICENSE).
