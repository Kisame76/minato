# Releasing

How a version of Minato goes out. A pushed version tag is the release: the workflow builds the GitHub release from
the CHANGELOG, and `tools/release.sh` does everything around it.

## Versions

- Betas: `v1.0.0-beta.1`, `v1.0.0-beta.2` and so on. GitHub marks them as pre-releases.
- Final versions: `v1.0.0`, then Semantic Versioning as users feel it. A patch has fixes and apps that start to
  work. A minor version adds formats, engines or features. A major version needs something users must do
  themselves, such as a higher minimum iOS version.
- In the app, the version is the plain number (`MARKETING_VERSION`, Apple takes only numbers), and a beta adds
  its label (`MINATO_RELEASE_LABEL`), so About shows "1.0.0 (Beta 1)".
- The build number (`CURRENT_PROJECT_VERSION`) grows by one with every release. SideStore and AltStore tell the
  betas of one version apart by it.

## The changelog

`CHANGELOG.md` follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Every change that users notice
adds a bullet under `## Unreleased`, in the same commit, under Added, Changed or Fixed.

- Write what works now that did not, as users see it: "Apps that play Ogg sounds through SoundPool have sound".
- Do not write internals.
- When an issue led to the change, end the bullet with its number, `(#123)`.

## Making a release

1. Run the release checks on the commit to release:
   - the full test suite (`docs/TESTING.md`);
   - both iOS slices;
   - a short run on an iPhone with the project's own test APKs.
2. In a clean clone of `main`, with the sources fetched and a full build made there (`docs/BUILDING.md`: Bionic, ART,
   hwui, the framework, framework-native and the Mac's apk-convert, which the app's embed steps read and release.sh
   does not build), run:

   ```bash
   tools/release.sh 1.0.0-beta.2
   ```

   The script first runs `tools/check-guest-fresh.py`, which refuses to go on when a guest output it packs
   (`build/bionic`, `build/art`, `build/framework`, `build/framework-native`, `build/hwui`, `build/graphics/guest`)
   is missing or older than the sources it is built from. A clone has fresh sources, so the full build has to be made in
   the clone; libraries left over from another tree (a link to the main tree's `build/`) are refused.

   The script:
   - moves the Unreleased section under `## 1.0.0-beta.2 - <date>`;
   - sets the version, the build number and the label in `project.yml`;
   - commits `chore: release v1.0.0-beta.2`;
   - builds the Release IPA, tags `v1.0.0-beta.2` and pushes.
3. The tag starts `.github/workflows/release.yml`. It reads the version's CHANGELOG section and creates the GitHub
   release with it. A tag without a section fails there. Only the highest final version becomes "latest".
4. The script waits for the release, then uploads five files to it:
   - `Minato.ipa`;
   - `Minato.ipa.sha256`;
   - `source-entry.json`;
   - `Minato-<version>-source.tar.gz` and its `.sha256`: the source of the parts under the GPL-2.0 with the Classpath
     Exception (`THIRD_PARTY.md`), which the license wants next to the IPA (section 3(a)). The script makes it before
     the IPA, from the pinned AOSP trees and our build scripts (`tools/make-source-archive.py`), and the app's
     `Licenses/*/SOURCE.txt` name its page and SHA-256.
5. `source-entry.json` is the new version's entry for the website's SideStore and AltStore source. Its version,
   build number, entitlements and privacy texts are read from the built app (`tools/release-entry.py`: every
   `NS*UsageDescription` key of the Info.plist, none listed by hand), because AltStore refuses an install
   whose IPA does not match them. It goes at the top of the website's `data/releases.json`.

`tools/release.sh VERSION --dry-run` builds the IPA, the entry and the source archive into `build/release` and changes
nothing; it makes the archive a second time and stops if the bytes differ.

The IPA is built on a Mac. The build needs AOSP's ART, ANGLE, MoltenVK and Bionic, too much for a hosted runner.
It is signed ad hoc, and SideStore or AltStore sign it again with the user's certificate.

## Tags

Version tags are protected against deletion and rewriting: a version someone installed keeps meaning what it meant.
To rebuild a release whose notes came out wrong, fix the CHANGELOG on `main` and run the release workflow by hand
for the existing tag.

## App reports

Users report apps that do not work with the issue form "An app does not work". In Minato, Report a problem fills
it in. A report goes through these steps:

1. Triage: the report is matched to a failure class (a missing API, an engine, graphics, sound, input).
2. Reproduction: the class is rebuilt with the project's own test app and checked against Android's output. A
   reporter's APK never enters the repository.
3. The fix lands with a changelog bullet that carries the issue number.
4. The issue is closed as fixed in the next beta.
