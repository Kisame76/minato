# Fremder Code

Was APK-Runner von anderen übernimmt, mit Lizenz und Stand. Projektlizenz: Apache-2.0 (ADR-0017). GPL- und AGPL-Code wird nicht übernommen.

## Gast-libc (ADR-0009, ADR-0021)

AOSP-Quellen zum Tag `android-15.0.0_r1`, von `android.googlesource.com`. `tools/fetch-sources.py` lädt sie nach `build/sources/` (nicht im Repo) und prüft sie gegen die Prüfsummen in [`third_party/sources.json`](third_party/sources.json). `tools/build-bionic.sh` baut daraus mit dem NDK `libc.so`, `libm.so`, `libdl.so` und `ld-android.so`. Diese Bibliotheken laufen als Gast-Code im Prozess.

| Quelle | Commit | Lizenz | Was davon |
|---|---|---|---|
| `platform/bionic` | `361ba86734fb` | BSD-artig und Apache-2.0, je Datei (`libc/NOTICE`) | libc, libm, libdl; Build-Beschreibungen, Generatoren (`gensyscalls.py`, `generate-version-script.py`); aus linker64 `linker/arch/arm64/tlsdesc_resolver.S` (BSD), in unsere `libc.so` gebaut |
| `platform/external/scudo` | `67dd481e24c7` | Apache-2.0 WITH LLVM-exception | Allocator der libc |
| `platform/external/gwp_asan` | `df96c8131b0e` | Apache-2.0 WITH LLVM-exception | von der libc verlangt |
| `platform/external/arm-optimized-routines` | `514df029da8a` | MIT OR Apache-2.0 WITH LLVM-exception | `string/` und `math/`: String- und Mathe-Routinen für arm64 |
| `platform/system/core` | `fc7bc8c4bfb4` | Apache-2.0 | 8 Dateien: libcutils-Header, Property-Parser |
| `platform/build` | `3d9c9aacdb6c` | Apache-2.0 | `fs_config_generator.py`: erzeugt die Tabelle der Android-UIDs |

**Unsere Änderungen an Bionic** (`runtime/bionic/`, ADR-0021):
- `overlay/bionic/libc/arch-arm64/bionic/setjmp.S`: Bionics Datei, ohne das Sichern und Zurückschreiben von `x18` (ADR-0019). Lizenz wie das Original (BSD, Kopf in der Datei).
- `src/libc_start.cpp`, `src/libc_start.map`, `src/sigreturn.S`, `src/ld_android.cpp`, `src/ld_android.map`: eigener Code (Apache-2.0), gebaut mit Bionics privaten Headern.
- `src/liblog.cpp`, `src/libnetd_client.cpp`: eigene kleine Fassungen von `liblog.so` und `libnetd_client.so` (Apache-2.0), nach der Dokumentation des NDK bzw. nach dem, was Bionic von der Library erwartet; kein AOSP-Code übernommen.

**Aus dem Android NDK r30** (`30.0.16248370`) in den Gast-Bibliotheken:

| Teil | Lizenz | Wofür |
|---|---|---|
| LLVM libunwind (`libunwind.a`) | Apache-2.0 WITH LLVM-exception | in `libc.so`, wie Androids `libunwind-exported`; `tools/export-archive-symbols.py` macht seine Symbole exportierbar |
| compiler-rt builtins | Apache-2.0 WITH LLVM-exception | Laufzeithilfen des Compilers (z. B. 128-Bit-Gleitkomma) |
| `crtbegin_so.o`, `crtend_so.o` | BSD (Bionic) | Start und Ende von `libm.so`, `libdl.so`, `ld-android.so` |
| `android/log.h` | Apache-2.0 | Header für die libc (liblog) |

## Zeitzonen

Bionic liest Zeitzonen aus einer Datei in seinem eigenen Format. `tools/make-tzdata.py` packt sie beim Build aus der IANA-Zeitzonendatenbank des Macs (`/usr/share/zoneinfo`, gemeinfrei) nach `build/bionic/system/usr/share/zoneinfo/tzdata`; nicht im Repo.

## Referenzen ohne übernommenen Code

- **ARTBox** (MIT, `fd48d48`): Die Liste der Bionic-Quellen und ihre Commits für `android-15.0.0_r1` diente als Ausgangspunkt; die Commits sind gegen den Tag auf `android.googlesource.com` geprüft. Code ist nicht übernommen.
