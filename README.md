# Minato

Android-Apps und -Spiele (APKs) nativ auf dem iPhone ausführen, ohne Emulator. Privates Forschungsprojekt, verteilt nur per Sideload. Vorher Arbeitstitel APK-Runner (ADR-0024).

- Architektur: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)
- Entscheidungen (ADRs): [`docs/DECISIONS.md`](docs/DECISIONS.md)
- Stand und nächste Schritte: [`docs/STATUS.md`](docs/STATUS.md)

## Bauen

Voraussetzungen: Xcode 27, XcodeGen, CMake und Ninja (`brew install xcodegen cmake ninja`), Android NDK r30 unter `~/Library/Android/sdk/ndk/30.0.16248370` (anderer Ort: `ANDROID_NDK` setzen).

```bash
tools/sim.sh                 # im Simulator bauen und starten
tools/deploy.sh              # auf dem angeschlossenen iPhone bauen, installieren und starten; übergibt dabei das Signier-Zertifikat
tools/gen.sh                 # nur das Xcode-Projekt erzeugen, z. B. vor dem Öffnen in Xcode
tools/build-native.sh --app  # C/C++ für die App bauen (build/APKRuntime.xcframework, build/APKConvert.xcframework), vor dem ersten Build in Xcode
```

`Minato.xcodeproj`, `app/Info.plist` und `app/Minato.entitlements` werden aus [`project.yml`](project.yml) erzeugt und nicht versioniert. Änderungen daher in `project.yml` vornehmen, nicht in Xcode.

## Tests und Werkzeuge

```bash
swift test --package-path importer/APKKit                      # Unit-Tests (Parser und Library)
swift run --package-path importer/APKKit apk-inspect <apk>     # zeigt, was die App aus einer APK liest
tools/apk-check.py <apk>                                        # Echtheit prüfen, bevor eine APK benutzt wird
tools/build-native.sh                                           # C/C++: NDK-Fixtures, Tests auf dem Mac (CTest), XCFramework
build/native/macos/importer/converter/apk-convert inspect <so>  # zeigt, was der Loader in einer ELF-Datei sieht, samt Befunden im Code (TPIDR_EL0, svc, x18)
build/native/macos/importer/converter/apk-convert convert <elf> <dylib> --sign -  # Container-Dylib schreiben und ad hoc signieren (ohne --sign unsigniert)
build/native/macos/importer/converter/apk-convert sign <dylib> --sign <p12> --identifier <Bundle-ID> --chain-from <signierte App-Datei>  # mit Zertifikat signieren, wie die App auf dem iPhone
build/native/macos/runtime/apk-run [Optionen] <dylib>           # Programm im Container starten (Optionen unten)
tools/build-bionic.sh                                           # Gast-libc: AOSP-Quellen holen (7,2 MB, geprüft), libc.so, libm.so, libdl.so, liblog.so bauen, Zeitzonen packen
tools/android-reference.sh                                      # dieselben Tests auf dem Android-Emulator, Vergleich mit fixtures/expected (ADR-0014)
tools/device-suite.sh                                           # dieselben Tests auf dem angeschlossenen iPhone, ohne Debugger (M2 Schritt 6)
tools/device-suite.sh --sign-on-device                          # ebenso, aber die App konvertiert und signiert selbst (M2b, ADR-0022)
tools/device-suite.sh --simulator [--sign-on-device]            # dieselben Läufe im iOS-Simulator
tools/hand-over-certificate.sh [--force]                        # Signier-Zertifikat vom Mac in die App auf dem iPhone, wenn es ihr fehlt (deploy.sh ruft es auf)
```

`APKRT_TRACE_SYSCALLS=1` vor `apk-run` zeigt jeden Linux-Syscall des Programms mit Argumenten und Ergebnis. Optionen von `apk-run`:

- `--mount <gast>=<host>` legt ein Host-Verzeichnis unter einen Android-Pfad; sonst sieht das Programm nur `/dev/null`, `/dev/zero` und die Zufallsgeräte.
- `--libraries <gast>=<host>` macht die Container eines Host-Ordners zu den Libraries eines Android-Ordners, z. B. `/system/lib64` für Bionic. Die Container neben dem des Programms liegen in dessen Ordner.
- `--cwd <gast>` setzt das Arbeitsverzeichnis, `--env <name>=<wert>` ergänzt die Umgebung, `--argv0 <text>` sagt, wie das Programm gestartet wurde.

Gesucht wird wie bei linker64: erst in `LD_LIBRARY_PATH`, dann in `/system/lib64`. Die Tests starten jedes Programm wie Androids Shell mit `--cwd /data/local/tmp --env LD_LIBRARY_PATH=/data/local/tmp --argv0 ./<name>`.

Der C/C++-Teil (`runtime/`, `importer/converter/`) baut mit CMake-Presets ([`CMakePresets.json`](CMakePresets.json)), die NDK-Testprogramme in `fixtures/` baut `tools/build-fixtures.sh`. Die Testsuite des M2-Plans liegt in `fixtures/suite/`, ihre erwarteten Ausgaben in `fixtures/expected/`; Welche Tests es gibt und wie jeder läuft, steht in `fixtures/tests.txt`. CTest lässt jedes Programm wie aus Androids Shell in `/data/local/tmp` laufen, `tools/android-reference.sh` vergleicht dieselben Läufe mit Android 15 im Emulator (ADR-0014), und `tools/device-suite.sh` führt sie auf dem iPhone aus: Die App startet dann je Test mit `-runFixture <test>`, die Programme liegen als signierte Container im Bundle. Mit `--sign-on-device` wandelt die App die ELF-Dateien selbst um und signiert sie mit dem Zertifikat in ihrem Schlüsselbund. Dorthin bringt es `tools/deploy.sh` von selbst: das Zertifikat, mit dem Xcode die App signiert (ADR-0022). Von Hand geht es im Menü „…“ → „Signing Certificate“ mit einer `.p12`-Datei. Die Gast-libc ist AOSP-Bionic (`android-15.0.0_r1`): `tools/fetch-sources.py` holt die Quellen nach `build/sources/` und prüft sie gegen [`third_party/sources.json`](third_party/sources.json), `tools/build-bionic.sh` baut sie nach `build/bionic/lib/`. Fremder Code steht in [`THIRD_PARTY.md`](THIRD_PARTY.md).

Test-APKs liegen lokal in `apk/` (von Git ignoriert), siehe [`docs/TEST-APKS.md`](docs/TEST-APKS.md).

## Lizenz

Apache-2.0, siehe [`LICENSE`](LICENSE).
