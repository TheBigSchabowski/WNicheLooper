# WNicheLooper (Windows)

Windows-Port von [NicheLooper](https://github.com/TheBigSchabowski/NicheLooper):
ein Live-Looper mit Metronom und Drum-Machine für Gitarre über ein
Audio-Interface (z. B. Roland Rubix44). Drei umschaltbare
**VST3**-Effekt-Chains (Tasten A / S / D) sitzen vor dem Looper, sodass der
Loop den Amp-Sound (NAM, TONEX, Archetypes, …) mit aufnimmt.

Gebaut mit Kotlin + Compose Multiplatform for Desktop (UI) und einer
C++-Audio-Engine über JNI. Audio-I/O wahlweise über **ASIO** (empfohlen —
niedrige Latenz, Pflicht für Amp-Sims) oder **WASAPI** (Fallback ohne
ASIO-Treiber).

## Status

⚠️ **Frisch portiert, auf Windows noch ungetestet.** Der Code ist eine
1:1-Portierung der macOS-App (Looper-Kern unverändert); Audio-I/O
(ASIO/WASAPI), VST3-Hosting und M4A-Import (Media Foundation) sind neu
geschrieben und brauchen reale Tests mit Interface + Plugins.

## Architektur

| Schicht | Technologie |
|---|---|
| UI | Kotlin, Compose Multiplatform for Desktop (Material 3) |
| Looper-Kern (C++) | `LooperEngine`, `RhythmSection` (`native/`, identisch zu macOS) |
| Audio-I/O (C++) | ASIO (`native/AsioBackend.cpp`, eigener Host) + WASAPI via [miniaudio](https://miniaud.io) |
| Bridge | JNI (`native/jni_bridge.cpp`) |
| Plugins | 3× VST3-Chain vor dem Looper (`native/VstPluginChain.cpp`, VST3 SDK), Tasten A/S/D |
| Loop-Dateien | verlustfreies Float32-WAV nach `%USERPROFILE%\Music\NicheLooper`; M4A-Import via Media Foundation |

Der Echtzeit-Audiopfad (Callback → Mono-Downmix → Chain → `LooperEngine::process`
→ Monitor-Mix → Limiter) läuft komplett nativ.

## Bauen (Windows)

Voraussetzungen:

1. **Visual Studio 2022 Build Tools** mit Workload „Desktopentwicklung mit C++“
   (der Gradle-Task findet MSVC automatisch über `vswhere`/`vcvars64`).
2. **JDK 17–21** (`JAVA_HOME` gesetzt). Das Compile-JDK 21 lädt Gradle bei
   Bedarf selbst (Foojay-Resolver).
3. Git-Submodule (VST3 SDK):

```bat
git clone --recursive https://github.com/TheBigSchabowski/WNicheLooper
:: oder nachträglich, nur die benötigten Submodule (spart vstgui/doc):
git submodule update --init third_party/vst3sdk
cd third_party\vst3sdk
git submodule update --init base pluginterfaces public.sdk
```

Das **ASIO SDK** (GPLv3-Option) liegt gebündelt unter `third_party/asiosdk`.

Dann:

```bat
gradlew.bat run          :: App direkt aus dem Source starten
gradlew.bat packageMsi   :: Installer (braucht WiX Toolset auf dem PATH)
```

## Bedienung

1. Backend wählen: **ASIO** (Interface-Treiber, ein Treiber für In+Out;
   „PANEL“ öffnet dessen Einstellungen) oder **WASAPI** (Input/Output
   getrennt wählbar).
2. **START ENGINE** drücken.
3. REC / SET LOOP / OVERDUB wie gewohnt; Metronom, Drums, Count-in,
   Auto-Loop und alle Regler identisch zur macOS-Version.
   Taktart (4/4, 3/4, 2/4, 6/8) setzt Taktlänge und Klick-Akzente, das
   **Groove**-Menü darunter das Drum-Pattern innerhalb des Takts. Die
   Groove-Liste zeigt nur Patterns der gewählten Taktart; ein Taktart-Wechsel
   springt automatisch auf deren ersten Groove. Beide Tabellen stehen in
   `native/RhythmSection.cpp` — ein neuer Groove ist ein Eintrag in
   `kGrooves`, die UI liest Namen und Zuordnung über JNI aus.
   - **Count-in** ist immer genau 2 volle Takte und startet das Drum-Pattern
     beim REC-Druck neu auf Zählzeit 1. Ohne Count-in bleibt es beim alten
     Verhalten: REC klinkt sich auf die nächste Taktlinie ein, ohne das
     Raster zu verschieben.
   - **Mute (`M`)** schaltet nur die Drums stumm, die Takt-Clock läuft
     weiter — der Wiedereinstieg landet also immer im Groove. Bereits
     angeschlagene Schläge klingen aus; das Metronom bleibt hörbar.
4. **Achtung Feedback:** Bei Mikrofon + Lautsprechern (WASAPI) den
   „Monitor input“-Schalter ausschalten.
5. **Amp-Sound hörbar machen:** Hardware-Direct-Monitoring am Interface
   zudrehen, „Monitor input“ in der App an. Meter: **In** = roh vom
   Interface, **FX** = nach der Chain, **Out** = Summe nach dem Limiter.

## Plugin-Chains (A / S / D)

Drei umschaltbare Effekt-Chains sitzen **vor** dem Looper:
Gitarre → aktive Chain → Loop-Aufnahme + Monitor.

- Gehostet werden die **VST3**-Builds der installierten Plugins aus den
  Standard-Ordnern (`C:\Program Files\Common Files\VST3` u. a.).
- **Tasten `A` / `S` / `D`** (oder die Chips) schalten die Live-Chain
  knackfrei um. **`M`** (oder der Mute-Chip) blendet die Drums live aus.
  Die Tasten greifen nur, wenn kein Textfeld den Fokus hat.
- „+ ADD PLUGIN“ fügt der aktiven Chain ein Plugin hinzu; **EDIT** öffnet
  das Original-Plugin-Fenster — dort z. B. das .nam-Modell laden.
- **Presets** (oben rechts) speichern alle 3 Chains inkl. komplettem
  Plugin-State nach `%APPDATA%\NicheLooper\presets`.

## Loops

- SAVE schreibt verlustfreies Float32-WAV nach `%USERPROFILE%\Music\NicheLooper`.
- LOAD liest WAV **und** M4A — vom Handy kopierte `Loop_*.m4a` einfach in
  denselben Ordner legen (Dekodierung über Media Foundation).

## Lizenzen & Drittanbieter-Inhalte

Dieses Repository steht unter der **GPLv3** (siehe `LICENSE`). Gebündelte
Drittanbieter-Komponenten behalten ihre jeweiligen Lizenzen:

- **VST3 SDK** (`third_party/vst3sdk`, git-Submodule) — **MIT**.
  © Steinberg Media Technologies GmbH. <https://github.com/steinbergmedia/vst3sdk>
- **ASIO SDK** (`third_party/asiosdk`) — genutzt und weitergegeben unter der
  **GPLv3**-Option der Dual-Lizenz. © Steinberg Media Technologies GmbH.
  ASIO ist eine Marke der Steinberg Media Technologies GmbH.
- **miniaudio** (`native/miniaudio.h`) — public domain oder MIT-0.
  © David Reid. <https://miniaud.io>
- **Drum-Samples** (`src/main/resources/drums/*.raw`) — aus dem Hydrogen
  Drumkit **„The Black Pearl 1.0“** von Glen MacArthur (AVL Drumkits),
  lizenziert unter **GPL**; Details in
  `src/main/resources/drums/ATTRIBUTION.txt`.

## Danksagung

Original-Konzept und Looper-Kern stammen aus der Android-/macOS-Version von
NicheLooper; dieser Port ersetzt Audio-I/O, Plugin-Hosting und Packaging
für Windows.
