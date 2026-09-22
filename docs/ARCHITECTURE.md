# APK-Runner – Architektur

Stand: M0, 2026-09-21; Spike-Ergebnisse ergänzt 2026-09-22. Entscheidungen stehen in [`DECISIONS.md`](DECISIONS.md) (ADR-0001 bis ADR-0018), Fortschritt in [`STATUS.md`](STATUS.md). Die Rohberichte der Recherche mit allen Quellen liegen in [`research/`](research/).

Markierungen: **[belegt]** = in Primärquelle gelesen oder gemessen, **[abgeleitet]** = eigene Schlussfolgerung, **[unbestätigt]** = Sekundärquelle oder Nutzerbericht.

## 1. Kurzfassung

- **Weg der `.so` beim Import:**
  - Android-`.so` laufen direkt auf der iPhone-CPU.
  - Beim Import wird jede `.so` geprüft und an wenigen, genau definierten Stellen umgeschrieben (Thread-Pointer-Zugriffe, Linux-Syscalls).
  - Danach wird sie in eine Mach-O-Dylib verpackt, mit dem eigenen Entwicklerzertifikat signiert und im Spiel per `dlopen` geladen (ADR-0008, ADR-0010).
- **Zwei Welten im Game-Prozess:**
  - Die **Gast-Welt** läuft mit Android-ABI: echte AOSP-Bionic, die `.so` der App und unsere Android-Systembibliotheken.
  - Die **Host-Welt** läuft mit Darwin-ABI: Loader, Linux-Syscall-Schicht, ANGLE, MoltenVK, AudioUnit und UIKit.
  - Übergänge gibt es genau zwei: Syscalls und die NDK-System-APIs (ADR-0009, ADR-0011).
- **Belegt ist:** Signieren mit dem eigenen Zertifikat und `dlopen` funktionieren auf iOS 27 (LiveContainer, JIT-less-Modus). Unsere eigenen Tests bestätigen das auch für Dateien im Datencontainer, wenn die Signatur-Kennung die Bundle-ID der App ist (S1b). Mit einer anderen Kennung lehnt iOS die Kopie ab (S1, ADR-0008).
- **Nicht belegt ist,** dass Android-`.so` auf einem echten iPhone laufen. ARTBox hat das bisher nur auf macOS in CI gezeigt. Deshalb laufen drei kleine Machbarkeitstests (Spikes) noch in M0 (siehe [`STATUS.md`](STATUS.md)).
- **Größte Risiken:**
  - Thread-Pointer in praktisch jeder `.so`.
  - Speicher und Adressraum mit kostenlosem Account.
  - ART-Heap gegen die 4-GiB-`__PAGEZERO` (M4).
  - OpenGL ES ≥ 3.1: ANGLE-Metal kann nur 3.0.
  - Fehlendes Android-Referenzgerät für den Performance-Vergleich.

## 2. Plattform-Fakten, die das Design bestimmen

| # | Fakt | Folge für uns | Beleg |
|---|---|---|---|
| F1 | iOS führt nur signierten Code aus. `dynamic-codesigning` gibt es nur für Apple. Auf A15+ (TXM, also auch A17 Pro) wird frisch geschriebener Code nur ausführbar, wenn ein angehängter Debugger die Seite beschreibt. | Aller Gast-Code muss vor dem Start als signierte Datei vorliegen. JIT gibt es nur optional über StikDebug. | [belegt] Apple Platform Security; StikJIT `INTEGRATION.md` |
| F2 | Library Validation: Ein Prozess darf Bibliotheken derselben Team-ID laden. | Konvertierte `.so` werden mit einem Zertifikat derselben (kostenlosen) Team-ID signiert. Ein Bypass ist nicht nötig. | [belegt] Apple „App code signing process"; LiveContainer JIT-less auf iOS 27. Eigene Tests: aus dem Bundle ja; Kopie im Datencontainer nur mit der Bundle-ID als Signatur-Kennung (S1/S1b, ADR-0008) |
| F3 | XNU überschreibt `TPIDR_EL0` bei jedem Kontextwechsel mit CPU- und Cluster-Bits. Der Darwin-Thread-Pointer liegt in `TPIDRRO_EL0`, das für Apps nur lesbar ist. | Android-Code liest den Thread-Pointer über `TPIDR_EL0`: der NDK-Standard-Stack-Protector bei TP+0x28 und ELF-TLS ab minSdk 29. Das funktioniert nicht unverändert, also wird es beim Import umgeschrieben (ADR-0010). | [belegt] xnu-12377.121.6 `osfmk/arm64/pcb.c:401–425`; LLVM `AArch64ISelLowering.cpp:33570`; auf macOS 27 und mit Spike S3 auf dem iPhone (iOS 27) gemessen |
| F4 | Der Kernel nullt `x18`, außer mit einem Apple-privaten Entitlement. | Gast-Code darf `x18` nicht nutzen. LLVM reserviert `x18` für Android ohnehin. Nutzer von `x18` sind inkompatibel. | [belegt] xnu `locore.s`, `machine_task.c`; LLVM `AArch64TargetParser.cpp:145` |
| F5 | Darwin nimmt die Syscall-Nummer aus `x16`, Linux aus `x8`. | Ein unverändertes Linux-`svc #0` löst einen zufälligen Darwin-Syscall aus. Jedes `svc` muss beim Import ersetzt werden. | [belegt] xnu `osfmk/arm64/sleh.c` |
| F6 | arm64-Programme brauchen eine harte 4-GiB-`__PAGEZERO`. Unter 4 GiB lässt sich nichts mappen. | Gast-Code kommt als Dylib in unseren Prozess. ARTs 32-Bit-Objektreferenzen (Heap unter 4 GiB) brauchen in M4 heap-relative Referenzen. | [belegt] xnu `bsd/kern/mach_loader.c:1087`; ARTBox ADR 0031 |
| F7 | iOS hat 16-KB-Seiten. NDK r28+ richtet `.so` standardmäßig auf 16 KB aus. | Auf 4 KB ausgerichtete `.so` (ältere Spiele) brauchen Sonderbehandlung oder werden abgelehnt (ADR-0007). | [belegt] developer.android.com „16 KB page sizes"; NDK-r28-Changelog |
| F8 | Apples ARM64-ABI weicht von AAPCS64 ab: Varargs liegen immer auf dem Stack, `va_list` ist `char*`, der Aufrufer erweitert Argumente unter 32 Bit, `long double` ist 64 Bit breit, `char` ist signed, Stack-Argumente sind gepackt. | Keine variadischen Aufrufe über die Grenze. Kleine Integer-Typen brauchen Thunks. Die Gast-Welt behält vollständig die Android-ABI (ADR-0011). | [belegt] Apple „Writing ARM64 code for Apple platforms" |
| F9 | errno-, Signal- und Flag-Werte unterscheiden sich, bei errno 54 von 87. Auch Strukturen (`stat`, `dirent`, `sigaction`, `ucontext`), pthread-Typgrößen und das `FILE`-Layout weichen ab. | Wir übersetzen an einer schmalen Stelle (Syscall-Schicht) statt in Hunderten libc-Funktionen (ADR-0009). | [belegt] Header-Vergleich Bionic-UAPI gegen iOS-27-SDK |
| F10 | Darwin-`CLOCK_MONOTONIC` zählt im Ruhezustand weiter, Linux-`CLOCK_MONOTONIC` nicht. | Gast-`CLOCK_MONOTONIC` wird auf Darwin-`CLOCK_UPTIME_RAW` abgebildet, `CLOCK_BOOTTIME` auf Darwin-`CLOCK_MONOTONIC`. | [belegt] `man clock_gettime` (macOS 27) |
| F11 | iOS kennt keine Subprozesse. App-Extensions sind eigene Prozesse, haben aber ein deutlich kleineres Speicherlimit und kosten App-IDs. | Ein Spiel läuft im Vordergrund-Prozess. Keine Extensions (ADR-0003). | [belegt] Apple Extensibility Guide; SideStore |
| F12 | Speicher auf dem 15 Pro Max unter iOS 27: 3,29 GiB verfügbar ohne Entitlement, 6,00 GiB mit `increased-memory-limit`. Adressraum: 7,84 GiB ohne (größte Region 6 GiB), 65,80 GiB mit dem Entitlement (größte Region 64 GiB), ohne `extended-virtual-addressing`. | Das Gast-Budget liegt bei höchstens 5–6 GB. Allocator-Reservierungen werden zentral budgetiert (ADR-0012). | [belegt] Spike S2 auf dem iPhone, 2026-09-22 (mit Entitlement 1 Lauf); vorher LiveContainer-Issue #1559 |
| F13 | Kostenloser Account: Profile gelten 7 Tage, 3 Apps pro Gerät, 10 App-IDs pro 7 Tage. Kein Extended Virtual Addressing. | Eine Bundle-ID, keine Extensions. Die App wird wöchentlich neu signiert (ADR-0015). | [belegt] Apple „Compare memberships", „Supported capabilities" |
| F14 | ANGLE-Metal unterstützt höchstens OpenGL ES 3.0. Es fehlen `OES_EGL_image_external` und `eglPresentationTimeANDROID`. | Unity 6 (verlangt ES 3.1) und UE5 (ES 3.2) laufen nicht über ANGLE-Metal. Vulkan wird der Hauptpfad (ADR-0013). | [belegt] ANGLE `DisplayMtl.mm`, `mtl_common.h` |
| F15 | MoltenVK 1.4.2 hat weder Geometry Shader noch Transform Feedback. Swapchains gibt es nur in BGRA8, RGBA16F und 10 Bit, Android erwartet RGBA8. | Der Vulkan-Shim wandelt RGBA8 in BGRA8 um (ein Blit pro Frame). Titel mit Geometry Shadern sind inkompatibel. | [belegt] MoltenVK `MVKDevice.mm`; AOSP `swapchain.cpp` |
| F16 | Das iOS-27-SDK verlangt einen Launch Screen. Xcode 27 hat `ld64` entfernt. | `UILaunchScreen` kommt in die Info.plist. Die Mach-O-Container schreiben wir selbst (ADR-0008). | [belegt] Release Notes iOS 27 / Xcode 27 |
| F17 | Signalzustellung funktioniert ohne Mach-Exception-Ports: `EXC_BAD_ACCESS` wird zu `SIGSEGV`/`SIGBUS` mit `si_addr`, `sigaltstack` wird respektiert. | Die Crash-Handler der Spiele und ARTs implizite Null- und Stack-Checks sind machbar. Nötig ist dafür eine Übersetzung Darwin-`mcontext` ↔ Linux-`ucontext`. | [belegt] xnu `bsd/uxkern/ux_exception.c`, `bsd/dev/arm/unix_signal.c` |

## 3. Gesamtbild

```
Launcher-Modus                          Game-Modus (eigener Prozessstart, ADR-0003)
--------------                          -------------------------------------------
SwiftUI: Library, Import, Einstellungen Overlay: Beenden, FPS
Importer:                               GAST-WELT (Android-ABI, als signierte Mach-O-Dylibs geladen)
  APK/Splits parsen, prüfen               App-.so (beim Import umgeschrieben + signiert)
  .so scannen, umschreiben                libc++_shared u. a. aus der APK
  Mach-O-Container bauen                  Bionic: libc, libm, libdl
  signieren, ablegen                      Front-Libs der NDK-System-APIs:
                                            libEGL, libGLESv2, libvulkan, libandroid, liblog,
                                            libaaudio, libOpenSLES, libz, libnativewindow ...
                                          ab M4: ART + Framework-Klassen
                                               |                      |
                                          Syscall-Veneers        ABI-Thunks / direkte Bindung
                                               v                      v
                                        HOST-WELT (Darwin-ABI, Teil des App-Binaries)
                                          Loader/Linker (ELF-Semantik, dlopen/dlsym)
                                          Linux-Personality: VM, VFS, Threads, futex,
                                            Signale, Zeit, /proc, System-Properties
                                          ANGLE-Metal | MoltenVK | CAMetalLayer
                                          AudioUnit | UITouch/GameController | CADisplayLink
```

**Import (Launcher-Modus):**
1. Datei über „+“ oder „Öffnen mit" übernehmen: als Klon in den versteckten Ordner `Library/Application Support/incoming/`, das Original bleibt, wo es ist (ADR-0018).
2. APK parsen: ZIP, binäres `AndroidManifest.xml`, `resources.arsc`, Signaturblöcke v1 bis v3.1.
3. Kompatibilität prüfen:
   - Ist `arm64-v8a` vorhanden?
   - Fehlen Splits?
   - GLES-Version und Vulkan-Level;
   - GMS-Abhängigkeit, minSdk/targetSdk.
4. Jede arm64-`.so` scannen (spätestens ab M2):
   - PT_LOAD-Layout und Ausrichtung;
   - Relokationstypen, TLS-Modell;
   - Zähler für `TPIDR_EL0`-Lesen/Schreiben, `svc` und `x18`.

   Ergebnis ist „Bereit" oder „Inkompatibel: <Grund>".
5. Ab M2: umschreiben, Mach-O-Container erzeugen, signieren. Das Original-ELF bleibt liegen, damit bei einem Zertifikatswechsel neu signiert werden kann.

**Start (Game-Modus):**
1. Der Launcher merkt sich das Paket und startet die App neu.
2. Die Host-Welt wird initialisiert: Speicherbudget, VFS-Wurzeln, Signal-Handler.
3. Die Gast-Systembibliotheken werden geladen. Sie liegen im App-Bundle und sind schon beim Xcode-Build signiert.
4. Die `.so` der App werden geladen: `dlopen` der Container, Datenrelokationen, Konstruktoren.
5. Start des Spiels: in M3 ruft der NativeActivity-Host `ANativeActivity_onCreate` auf, ab M4 startet ART die `Activity`.
6. Beenden über das Overlay beendet den Prozess, danach startet die App neu in den Launcher.

## 4. Komponenten

### 4.1 App (`app/`, SwiftUI)
- **Aufgaben:** Library-Grid, Import, Launcher, Overlay.
- **Info.plist:**
  - `UILaunchScreen` (Pflicht mit dem iOS-27-SDK);
  - ab M1: `LSSupportsOpeningDocumentsInPlace`, `CFBundleDocumentTypes`, `UTImportedTypeDeclarations`, kein `UIFileSharingEnabled` (ADR-0018);
  - ab M3: `CADisableMinimumFrameDurationOnPhone`, `GCSupportsGameMode`, Kategorie Games.
- **Speicherorte:**
  - `Documents/` bleibt leer; nur iOS legt bei „Öffnen mit" Kopien in `Inbox/` ab, die die App übernimmt und löscht;
  - `Library/Application Support/incoming/` für gewählte Dateien bis zur Installation;
  - `Library/Application Support/apps/<package>/` für Installiertes (APKs, konvertierte und signierte Libs, Metadaten);
  - `Library/Application Support/data/<package>/` für App-Daten (siehe 4.5).
- **Bundle und Signatur:** eine Bundle-ID, keine App-Extensions (ADR-0003, ADR-0015).

### 4.2 Importer (`importer/`)
- **APKKit** (Swift-Paket):
  - ZIP, AXML, ARSC, APK-Signaturen v1–v3.1 (Zertifikat speichern, manche Apps prüfen ihre Signatur).
  - Datenmodell von Anfang an als *App = Base + n Splits + OBBs*.
  - Kompatibilitätsregeln.
  - Tests laufen mit `swift test` auf dem Mac. Als Test-Orakel dienen androguard und apk-info (beide Apache-2.0, nur zum Vergleichen).
- **Konverter** (C++, CMake):
  - Bestandteile: ELF-Scanner, Umschreiber (4.4) und Mach-O-Writer (ADR-0008).
  - Derselbe Code läuft auf dem Mac (M2a, Build-Werkzeug) und auf dem iPhone (M2b).
- **Signierer** (M2b): zsign (MIT, Upstream zhlynn) mit OpenSSL. Das Zertifikat liegt im iOS-Schlüsselbund.

### 4.3 Loader (`runtime/loader`)
- **Linker mit Android-Semantik:**
  - Lastgruppen, eager Binding;
  - Symbolversionen nach AOSP-Regeln, GNU- und SysV-Hash;
  - RELA/RELR/APS2, IRELATIVE, TLSDESC/IE;
  - `init_array`-Reihenfolge;
  - `dlopen`/`dlsym`/`dladdr`/`dl_iterate_phdr` (die letzten zwei für libunwind und C++-Exceptions);
  - keine Entladung.
- **Signierter Code bleibt unverändert:** Zur Laufzeit werden nur Datenbereiche geschrieben. `.so` mit Text-Relokationen sind inkompatibel.
- **Basis:** geprüfte ARTBox-Module (MIT): ELF-Parser, Relokations-Engine, Lastgruppen, Symbolversionen (ADR-0017). Ergänzt werden alles, was vorgebaute `.so` brauchen (≥ 3 PT_LOADs, APS2, IRELATIVE, IE-TLS), sowie das Wachsen per `dlopen`.

### 4.4 Umschreiben beim Import (ADR-0010)

| Muster | Vorkommen | Behandlung |
|---|---|---|
| `mrs xN, TPIDR_EL0` (lesen) | praktisch jede `.so`: Stack-Protector ist NDK-Standard, ELF-TLS ab minSdk 29 | Wird ein Sprung in eine Insel. Die Insel lädt den Gast-Thread-Pointer aus einem Darwin-TSD-Slot (`mrs xN, TPIDRRO_EL0; ldr xN, [xN, #slot]`) und springt zurück. Ein Fast-Path für das Stack-Guard-Muster wird in M2 gemessen. |
| `msr TPIDR_EL0, xN` (schreiben) | libc und Laufzeiten | In unserer Bionic per Quelltext-Overlay gelöst. In App-Code: „Inkompatibel". |
| `svc #0` | selten: Go-Runtime, LSS, statisch gelinkte libc | Wird ein Sprung in einen Veneer: Register sichern, Linux-Syscall-Schicht aufrufen, Linux-Fehlerkonvention beibehalten. |
| `x18` als Register | selten: alte GCC-Builds, ShadowCallStack | „Inkompatibel" |
| Text-Relokationen | selten | „Inkompatibel" |

- **Inselbereiche:** Sie liegen im Container vor und hinter dem Code, weil ein Sprung nur ±128 MB weit reicht. `.so` mit mehr als etwa 256 MB Code brauchen einen Sonderweg und werden zunächst als inkompatibel markiert.
- **Keine Emulation:** Nur eindeutig dekodierte Muster werden ersetzt, alles andere läuft unverändert nativ. Es gibt kein Trapping und kein Patchen zur Laufzeit.

### 4.5 Gast-libc und Linux-Personality (ADR-0009)
- **Gast-libc:** Echte AOSP-Bionic, mit dem NDK gebaut und als Gast-Bibliothek im App-Bundle mitgeliefert. Das Quelltext-Overlay folgt dem ARTBox-Rezept:
  - TLS-Zugriffe laufen über unseren Gast-Thread-Pointer;
  - Syscall-Stubs rufen die Host-Schicht auf;
  - ohne ShadowCallStack.
- **Syscall-Schicht (Host):** ein Mini-Kernel im Userspace. Linux-Nummern werden nie an Darwin durchgereicht.

| Bereich | Umsetzung |
|---|---|
| Speicher | eigener Adressraum-Manager: `mmap`/`munmap`/`mprotect`, `MADV_DONTNEED` per Neu-Mapping, `mremap` nachgebaut; ausführbare Mappings nie |
| Dateien | VFS mit Pfadabbildung (Tabelle unten); `openat`, `read`/`write`, `getdents64`, `stat`-Familie mit Linux-Strukturen |
| Threads | `clone` erzeugt einen Darwin-pthread (Reaper-Modell von ARTBox); futex über `os_sync_wait_on_address` (öffentlich, iOS 17.4+) |
| Signale | Host-`sigaction`; Zustellung mit Linux-`siginfo`/`ucontext` auf dem Gast-Altstack, danach Zurückschreiben der Register; Signalnummern übersetzt |
| Zeit | Clock-Abbildung aus F10 |
| Netz | Sockets auf BSD-Sockets; `epoll`/`eventfd`/`pipe2` über kqueue |
| System | `/proc/self/{maps,cmdline,exe,auxv}`, `/proc/cpuinfo` synthetisch; `AT_PAGESZ = 16384`; Property-Bereich (`ro.build.version.sdk` u. a.) passend zur mitgelieferten Bionic |

| Android-Pfad | Ziel auf dem iPhone |
|---|---|
| `/data/data/<pkg>`, `/data/user/0/<pkg>` | `Library/Application Support/data/<pkg>/data` |
| `/sdcard/Android/data/<pkg>` | `…/data/<pkg>/external` |
| `/sdcard/Android/obb/<pkg>` | `…/data/<pkg>/obb` |
| `/data/app/<pkg>-*/base.apk` (nur lesen) | `…/apps/<pkg>/base.apk` |
| `/system/lib64` | Gast-Systembibliotheken im App-Bundle |
| `/dev/null`, `/dev/zero`, `/dev/urandom` | virtuell |

### 4.6 ABI-Grenze Gast ↔ Host (ADR-0011)
- **Wege vom Gast zum Host:** Gast-Code erreicht den Host nur über Syscall-Veneers oder über Importe der Front-Libs.
- **Bindung:** Der Loader bindet eine Front-Lib-Funktion direkt an die Host-Funktion, wenn die Signatur in beiden ABIs gleich ist: nur Zeiger und Integer ≥ 32 Bit, höchstens 8 Register, nicht variadisch. Sonst läuft der Aufruf über einen generierten Thunk, der kleine Integer erweitert und Stack-Argumente umpackt. Die Tabellen werden aus Khronos-XML und NDK-Headern erzeugt.
- **Verboten über die Grenze:** variadische Aufrufe, `va_list` und `long double`.
- **Host → Gast** (Audio-Callback, Choreographer, Input): Diese Aufrufe laufen über Thunks. Die Thunks melden den Host-Thread bei Bedarf als Gast-Thread an (Gast-TCB, TLS, Signalmaske), ähnlich wie JNI `AttachCurrentThread`. Ohne diese Anmeldung stürzt der Gast-Code sofort ab, weil sein Stack-Protector keinen Gast-Thread-Pointer findet.
- **errno und Zeit:** Host-Fehler werden in Linux-errno übersetzt. Zeitwerte folgen der Clock-Abbildung aus F10.

### 4.7 Grafik (ADR-0005, ADR-0013)
- **`ANativeWindow`:** eine UIView mit `CAMetalLayer`. `setBuffersGeometry` setzt `drawableSize`, das ist später der Ansatz für MetalFX-Upscaling (M6).
- **EGL/GLES:** Die Front-Libs gehen auf ANGLE-Metal (ES ≤ 3.0). ANGLE bekommt einen kleinen Patch für `eglPresentationTimeANDROID` (für Swappy), `OES_EGL_image_external` folgt später. ES 3.1 kommt als Prototyp über ANGLE-Vulkan auf MoltenVK.
- **Vulkan:**
  - Die Front-Lib `libvulkan` geht in Release-Builds direkt auf MoltenVK. Den Khronos-Loader mit Validation Layers gibt es nur im Debug-Build.
  - `VK_KHR_android_surface` wird zu `VK_EXT_metal_surface`. `VK_KHR_portability_subset` wird automatisch aktiviert.
  - Swapchain: Blit RGBA8 → BGRA8, `currentTransform = IDENTITY`, höchstens 3 Images.
  - `AHardwareBuffer` wird auf IOSurface abgebildet, die fd-Extensions werden emuliert.
- **Hintergrund:** iOS verbietet GPU-Arbeit im Hintergrund. `onPause` und `APP_CMD_TERM_WINDOW` laufen deshalb synchron, und der Render-Thread des Spiels muss bestätigen, bevor die App in den Hintergrund geht.

### 4.8 Frame-Pacing, Input, Audio
- **Choreographer:**
  - CADisplayLink läuft auf einem eigenen Thread und weckt ALooper über fds.
  - Einstellungen: `preferredFrameRateRange` (80, 120, 120) und `CADisableMinimumFrameDurationOnPhone`.
  - Zeitstempel liegen auf der `mach_absolute_time`-Basis.
- **Swappy** (in vielen NDK-Spielen statisch gelinkt) braucht:
  - ein gemeldetes SDK ≥ 31;
  - funktionierende JNI-Methoden `Display`/`WindowManager`;
  - `VK_GOOGLE_display_timing` (hat MoltenVK);
  - `eglPresentationTimeANDROID` (ANGLE-Patch).
- **Input:**
  - `UITouch` inklusive Coalesced Touches wird zu `AMotionEvent` mit History, Pointer-IDs und Pixelkoordinaten.
  - `GCController` wird zu `AKeyEvent`/`AMotionEvent` mit den Android-Achsen.
- **Audio:**
  - Eigene `libaaudio` auf RemoteIO. Sie muss den Symbolsatz exportieren, den Oboe lädt.
  - OpenSL ES kommt aus AOSP `frameworks/wilhelm` (Apache-2.0) mit einem neuen AudioUnit-Backend.

### 4.9 ART und Framework (M4, Ausblick)
- **Ziel: ART als Gast-Bibliothek mit Android-ABI**, gebaut wie für ein Gerät. So bleiben JNI-Aufrufe mit ihren Varargs (`Call*Method(...)`) in einer ABI, und ART sieht über die Personality dieselbe Linux-Semantik wie der Rest. Die Alternative, ART für Darwin zu bauen, wird in M4 entschieden.
- **Konfiguration:**
  - Interpreter (`-Xint`, kein JIT);
  - imageless Boot aus den Boot-Jars;
  - GC ohne userfaultfd (kein CMC);
  - **heap-relative 32-Bit-Referenzen** wegen F6 (ARTBox-Ansatz, bisher nur isoliert getestet).
- **nterp** erzeugt keinen Code zur Laufzeit, sondern ist Teil von `libart` [abgeleitet]. Er fällt also nicht unter das JIT-Verbot, muss aber an die heap-relativen Referenzen angepasst werden.
- **Framework:** Wir schneiden wie ATL zwischen App und Java-Framework: Das Framework wird auf echtem ART nachgebaut, sauber neu geschrieben ohne ATL-Code. Keine Binder-Prozesse, System-Services sind In-Process-Objekte.
- **Lizenz:** Die Projektlizenz ist Apache-2.0 (ADR-0017). libcore steht unter GPL-2.0 mit Classpath-Exception und bleibt deshalb eine eigenständige Komponente.
- **M6:** Die dex2oat-Ausgabe durchläuft dieselbe Pipeline wie `.so` (konvertieren, signieren, `dlopen`). Der Compiler muss dafür heap-relative Referenzen erzeugen.

### 4.10 Prozessmodell (ADR-0003)
- **Neustart:** Der Launcher schreibt das Zielpaket in die UserDefaults, öffnet die eigene App über `LSApplicationWorkspace` (private SPI, wie LiveContainer) und beendet sich. Beim Start liest die App das Ziel und bootet direkt den Game-Modus.
- **Fallback:** Die App beendet sich, und der Nutzer tippt noch einmal auf das Icon.
- **Warum Neustart:** Pro Prozess läuft nur ein Spiel, weil geladene Libs und ART nicht sauber entladen werden können.

## 5. Referenzprojekte: was wir übernehmen, was nicht

| Projekt (Stand) | Lizenz | Übernehmen | Nicht übernehmen | Warum |
|---|---|---|---|---|
| ARTBox (`fd48d48`, 2026-09-14) | MIT | Nach Review als gepinnte Module: ELF-Parser, Relokations-Engine, Lastgruppen, Symbolversionen, VM-/futex-/VFS-Engines. Dazu das Bionic-Build-Rezept (Overlay von 6 Dateien, Syscall-Stubs), die Testdisziplin (Orakel, feste Nenner) und ab M4 das ART-Rezept. | Nur-Build-Zeit-Wrapper als Produktweg; kontrolliertes ELF-Layout (`--no-relax`, genau 2 PT_LOADs); TLS-Trick nur im Quelltext; Scudo mit 8,25 GiB VA; Verzicht auf Gerätetests | Bester Ausgangspunkt. Aber: 9 Tage alt, ein Autor, nie auf einem iPhone gelaufen, nur für selbst gebaute ELFs gedacht. |
| LiveContainer (`4dbe0f9`) | AGPL-3.0 | Nur Konzepte: Zertifikat-Import aus SideStore/AltStore, Co-Signing aller Mach-Os, `dlopen` aus dem Datencontainer, Neustart am Platz, `seg_count`-Toleranz für iOS 27 | Code; Multitasking über App-Extensions; private dyld-Hooks | Lizenz; Speicherlimits der Extensions |
| zsign (upstream, `614caa8`) | MIT | On-device-Signierer für M2b | Den Fork aus Feather/LiveContainer | Saubere Herkunft (Feather ist GPL-3.0) |
| Android Translation Layer (`3e686d1`) | GPL-3.0 | Nur Konzepte: Schnitt App ↔ Java-Framework, NativeActivity-Host per `dlopen`+`dlsym`, Einfügen von Surface- und Instance-Extensions bei Vulkan, Pfad-Virtualisierung | Jeglicher Code; GTK/Wayland/ALSA | Lizenz; falsche Plattform |
| bionic_translation (`b36c17d`) | Apache-2.0, Linker ohne Lizenz (aus android2gnulinux) | Idee eines TLS-Modulregisters | Code; der Shim-Ansatz | Unklare Herkunft; auf Darwin weichen alle Konstanten ab (F9) |
| art_standalone (`66a5d90`) | Apache-2.0 | Referenz, wie ART außerhalb von AOSP gebaut wird | Den Stand selbst (Android-10-Ära, GCC/Linux) | Veraltet |
| AOSP (Bionic, ART, libcore, wilhelm, apksig, gamesdk) | Apache-2.0/BSD; libcore GPL-2.0+CE | Bionic, ART, wilhelm; 4-KB-Kompatibilitätslogik aus `linker_phdr_16kib_compat.cpp`; Signaturformate | – | Referenzimplementierung |
| StikJIT (`3623e72`) / StikDebug | MPL-2.0 / AGPL-3.0 | Später, nur für einen optionalen JIT-Pfad: URL-Protokoll `stikdebug://enable-jit`, `brk #0xf00d` | Eingebautes StikJIT (braucht eine Extension) | ADR-0002 |
| bundletool / apkeep | Apache-2.0 / MIT | Referenzen für M5/M7 | – | – |
| ANGLE / MoltenVK | BSD-3 / Apache-2.0 | Abhängigkeiten ab M3, nur nach Rückfrage | MetalANGLE (inaktiv), Apples EAGL (veraltet, kein EGL) | – |
| KosmicKrisp | MIT | Beobachten: Vulkan 1.3 konform, hat Transform Feedback | Vorerst gar nicht | Keine iOS-Unterstützung (Stand 2026-08) |

## 6. Abhängigkeiten und Aufwand (Einbindung nur nach Rückfrage)

| Abhängigkeit | ab | Größe / Aufwand (geschätzt) | Lizenz |
|---|---|---|---|
| XcodeGen 2.46.0 | M0 | Homebrew, klein | MIT |
| CMake 4.4.3, Ninja 1.13.2 | M2 | Homebrew, klein | BSD-3 / Apache-2.0 |
| Android NDK r30 (30.0.16248370) | M2 | Download ca. 1,07 GB | Apache-2.0 u. a. |
| Android-Emulator über cmdline-tools | optional, M2 | mehrere GB | – |
| zsign + OpenSSL | M2b | OpenSSL-Build oder fertiges Paket, mittel | MIT / Apache-2.0 |
| AOSP-Bionic (gepinnter Tag) | M2 | Quellarchiv, Build mit NDK | BSD/Apache |
| MoltenVK 1.4.2 | M3 | fertiges XCFramework ca. 32 MiB oder eigener Build | Apache-2.0 |
| ANGLE | M3 | depot_tools + GN, Checkout mehrere GB, erster Build einige zehn Minuten | BSD-3 |
| ART + libcore + ICU | M4 | ca. 1.150 C++-Einheiten (ARTBox-Zählung), JDK 17 + D8, `libart` ca. 14 MB, ICU-Daten ca. 28 MB | Apache-2.0; libcore GPL-2.0+CE |

## 7. Risiken

| ID | Risiko | Stufe | Gegenmaßnahme |
|---|---|---|---|
| R1 | Thread-Pointer und TLS in allen vorgebauten `.so` (F3) | hoch | Umschreiber (ADR-0010); Spike S3 hat das XNU-Verhalten auf dem iPhone bestätigt |
| R2 | Konvertieren, Signieren und `dlopen` von Android-Code sind auf dem iPhone unbewiesen | hoch; nach S1b mittel | S1b: Eine Mac-signierte Dylib mit der Bundle-ID als Kennung lädt aus dem Datencontainer. M2a lädt aus dem Bundle; offen ist das Signieren auf dem iPhone mit zsign (M2b, ADR-0008) |
| R3 | Speicher und Adressraum mit kostenlosem Account (F12, F13) | hoch; mit Entitlement mittel | `increased-memory-limit` (S2: 6,00 GiB, 64-GiB-Region); Allocator-Reservierungen zentral budgetiert (ADR-0012) |
| R4 | ART-Heap über 4 GiB (F6), Interpreter langsam | hoch (M4/M6) | Heap-relative Referenzen; Spiele mit wenig Java (NativeActivity, IL2CPP) zuerst |
| R5 | GLES ≥ 3.1 nicht über ANGLE-Metal (F14) | hoch | Vulkan als Hauptpfad; Prototyp ANGLE-Vulkan (ADR-0013) |
| R6 | Kein Android-Referenzgerät, Parity nicht messbar | hoch (M3) | Entscheidung vor M3 (ADR-0014) |
| R7 | Fehler an der ABI-Grenze: kleine Integer, Callbacks auf Host-Threads (F8) | mittel | Generierte Thunks; Tests mit Müll in den oberen Bits |
| R8 | Auf 4 KB ausgerichtete `.so` älterer Spiele (F7) | mittel | Erst nur 16 KB; dann RW-Verschiebung plus ADRP-Korrektur, gesteuert durch Korpus-Daten (ADR-0007) |
| R9 | Signale: Crash-Handler von Unity/Mono/ART erwarten Linux-`ucontext` | mittel | Übersetzung in beide Richtungen in M2 |
| R10 | dyld-Interna und private SPIs ändern sich (iOS 27: `seg_count`) | mittel | Eigener Mach-O-Writer ohne Chained Fixups; SPI-Nutzung klein und kapseln; LiveContainer-Issues als Frühwarnung |
| R11 | Unsicherer Code-Stand von ARTBox | mittel | Nur einzelne Module, gepinnt, reviewt, mit eigenen Tests |
| R12 | Zertifikate von Xcode und SideStore kollidieren. Das Limit aktiver Entwicklungszertifikate kann zu einem Widerruf führen, und damit wird alles ungültig, was damit signiert ist. | mittel | Bis M2b nur Xcode; bei Zertifikatswechsel automatisch neu signieren |
| R13 | Performance-Kosten durch Inseln, Syscall-Schicht, Swapchain-Blit und Interpreter-ART | mittel | Messen in M2/M3; Fast-Paths |
| R14 | Neues Projektformat in Xcode 27.2 (XcodeGen #1651/#1653) | niedrig | Auf Xcode 27.0 bleiben, bis XcodeGen es unterstützt |
| R15 | Der Simulator täuscht: keine gleiche Signaturprüfung, kein Jetsam, ANGLE fest auf ES 3.0 | niedrig | Simulator nur für die UI; Loader-Tests auf macOS und dem iPhone |
| R16 | Lizenzen: AGPL/GPL-Referenzen, libcore | niedrig | ADR-0017: Projektlizenz Apache-2.0 (2026-09-22); AGPL/GPL nur lesen; libcore als eigenständige Komponente |

## 8. Offene Fragen

**Deine Entscheidung:**
- GLES-Pfad (ADR-0013, vor M3, berührt feste Entscheidung #5);
- Referenzgerät (ADR-0014, vor M3);
- Zeitpunkt, den Account eventuell zu bezahlen (ADR-0015);
- Bundle-ID.

**Prototypen:**
- Signieren auf dem iPhone mit zsign und dem exportierten Xcode-Zertifikat (M2b; ADR-0008);
- Häufigkeit von 4-KB-Layout, `TPIDR_EL0`, `svc` und `x18` in echten APKs (Korpus, M1/M2);
- Kosten der Inseln;
- Fast-Path für den Stack-Guard;
- ART mit heap-relativen Referenzen und nterp (M4);
- Kosten des Swapchain-Blits bei 120 Hz (M3).

## 9. Wichtigste Quellen

- **ARTBox:** github.com/j0shua-SYSON/ARTBox (`docs/loader-design.md`, `DECISIONS.md`, `STATUS.md`)
- **LiveContainer:** github.com/LiveContainer/LiveContainer (`LCMachOUtils.m`, `LCBootstrap.m`, `zsign.mm`), livecontainer.github.io
- **Android Translation Layer:** gitlab.com/android_translation_layer (`doc/Architecture.md`)
- **XNU:** github.com/apple-oss-distributions/xnu, Tag `xnu-12377.121.6` (`pcb.c`, `locore.s`, `sleh.c`, `mach_loader.c`, `ux_exception.c`)
- **Apple:** „Writing ARM64 code for Apple platforms", „Supported capabilities (iOS)", „Compare memberships", Release Notes iOS 27 / Xcode 27
- **LLVM:** `AArch64ISelLowering.cpp`, `AArch64TargetParser.cpp`
- **Android:** „Support 16 KB page sizes", NDK-Releases (r30), AOSP `bionic/linker/linker_phdr_16kib_compat.cpp`
- **Grafik:** ANGLE (`DisplayMtl.mm`, `SurfaceMtl.mm`), MoltenVK 1.4.2 (`MVKDevice.mm`, `MVKImage.mm`), Mesa KosmicKrisp-Doku
- Alle weiteren Quellen mit Commits: [`research/`](research/)
