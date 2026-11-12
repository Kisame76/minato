# Contributing to Minato

Thanks for your interest. Minato is a young project with strict rules about tests and licenses; they keep a runtime
that rewrites and signs other people's code trustworthy. Please read this page before you open a pull request.

## Reporting an app that does not work

Use the issue form [An app does not work](https://github.com/Kisame76/minato/issues/new?template=app-report.yml).
The form says what to include. Never attach APK files, and leave out anything personal.

## Proposing changes

- For anything larger than a small fix, open an issue first and describe the problem and the approach.
- The fixed principles (no emulation, no JIT in the core path, one app per process, the framework in-process,
  arm64-v8a only, 16 KB pages) and the recorded design decisions (numbered design records, called ADRs, indexed in
  [docs/DESIGN.md](docs/DESIGN.md)) change only through a new or amended record. Propose it in an issue, with the
  options you considered.
- If something provably cannot work on iOS, say so and show the alternatives rather than working around it quietly.
- Keep pull requests focused: one change, with its tests, its documentation and its changelog entry.

## Tests are required

Every change comes with tests, written first. See [docs/TESTING.md](docs/TESTING.md). Building Minato and running
them needs a Mac with Apple silicon, Xcode and the Android NDK, among other things: the full list and the steps
are in [docs/BUILDING.md](docs/BUILDING.md).

- **Red first:** the test fails before the change and passes after it. Say in the pull request how it failed.
- **Android is the reference:** expected output comes from Android 15 in the emulator
  (`tools/android-reference.sh`), never from Minato's own output. New expected files go to `fixtures/expected/`.
- **Run the whole suite:** `swift test --package-path importer/APKKit`, all of CTest (`ctest --preset macos`) and
  the check scripts your change touches. Look for skipped tests.
- **Both iOS slices:** code the app links must build for the device and the Simulator without warnings.
- **Risky changes** (parsers, signing, the loader, the rewriter, anything that handles untrusted input) also get a
  mutation check: break the change on purpose and confirm that a test catches it.

## Changelog and releases

Every change that users notice adds a bullet under `## Unreleased` in [CHANGELOG.md](CHANGELOG.md), in the same
commit, under Added, Changed or Fixed. Write what works now that did not, as users see it, not the internals.
Versions and releases are described in [docs/RELEASING.md](docs/RELEASING.md).

## Code style

- Lines are at most 120 columns, in code and in documents.
- Everything in the repository is in English: code, comments, commit messages and documentation.
- Commit messages follow [Conventional Commits](https://www.conventionalcommits.org/): `feat:`, `fix:`, `docs:`,
  `test:`, `refactor:`, `perf:`, `build:`, `ci:`, `chore:`, `style:`, then one or two plain sentences.
- Native code builds with warnings as errors; follow the style of the files around your change.
- Cite the design record (ADR) a piece of code implements, with its number and, where it helps, the decision
  inside it (for example "ADR-0045 decision 6"), where it explains why.
- The Xcode project is generated: change `project.yml`, not the project in Xcode.

## Licensing

- Minato is licensed under Apache-2.0, and contributions are accepted under the same license.
- **No GPL or AGPL code** goes into the repository. GPL and AGPL projects may be read for their concepts, not
  copied. The one GPL component, ART's class library (GPL-2.0 with the Classpath Exception), is fetched at build
  time and ships as a separate component.
- **Record every piece of third-party code** in [THIRD_PARTY.md](THIRD_PARTY.md) with its version, source and
  license before it lands. Downloaded sources are pinned with their digest in `third_party/sources.json`; when the
  app embeds something, its license text goes into `third_party/licenses.json` so that the app carries it.
- Check the license first. Large new dependencies need an issue and agreement before any work on them.

## Never in the repository

- **No APKs** or other app packages (`.apk`, `.apks`, `.xapk`, `.apkm`, `.aab`, `.obb`) of real apps, and no
  proprietary components such as Google Play services. Test apps are written for this project and built from
  source.
- **No proprietary or decompiled code,** and no work derived from reading decompiled proprietary libraries. Learn
  from documentation, open source and observed behavior on Android.
- **No signing certificates, private keys or provisioning profiles.**
- **No personal data:** no names, e-mail addresses, local paths, device names or identifiers, and no screenshots,
  recordings or logs from personal devices. Measurements are fine.
- **No real app names** in tests, code or documents; describe the case instead ("a Unity test APK").
- **Nothing that bypasses** Play Integrity, license checks, anti-cheat or DRM.
