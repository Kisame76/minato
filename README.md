# APK-Runner (Arbeitstitel)

Android-Apps und -Spiele (APKs) nativ auf dem iPhone ausführen, ohne Emulator. Privates Forschungsprojekt, verteilt nur per Sideload.

- Architektur: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)
- Entscheidungen (ADRs): [`docs/DECISIONS.md`](docs/DECISIONS.md)
- Stand und nächste Schritte: [`docs/STATUS.md`](docs/STATUS.md)

## Bauen

Voraussetzungen: Xcode 27 und XcodeGen (`brew install xcodegen`).

```bash
tools/sim.sh      # im Simulator bauen und starten
tools/deploy.sh   # auf dem angeschlossenen iPhone bauen, installieren und starten
tools/gen.sh      # nur das Xcode-Projekt erzeugen, z. B. vor dem Öffnen in Xcode
```

`APKRunner.xcodeproj` und `app/Info.plist` werden aus [`project.yml`](project.yml) erzeugt und nicht versioniert. Änderungen daher in `project.yml` vornehmen, nicht in Xcode.
