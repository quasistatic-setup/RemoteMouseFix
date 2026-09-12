# RemoteMouseFix: Projektregeln

Globale Regeln in `~/AGENTS.md` gelten zusätzlich und haben bei Git, Geheimnissen und
Dokumentationsform Vorrang vor Gewohnheiten. Diese Datei beschreibt nur, was für dieses
Repository eigen ist.

## Projektgrenzen

Portables Windows-Diagnosewerkzeug gegen fehlerhafte Maus- und Kamerasprünge in
3D-Spielen über Fernwartungssoftware. Erster Fall: TeamViewer mit WoW 3.3.5a.

Technisch festgelegt und nicht ohne ausdrücklichen Auftrag zu ändern:
C++17, Win32-API, CMake, Windows 10/11, zunächst x64.

Das Repository enthält ausschließlich Quellcode, Konfigurationsvorlage und
Dokumentation. Aufgezeichnete Logs sind Sitzungsdaten und gehören nie in Git.

## Übergreifende Schutzregeln

Diese Grenzen sind der Zweck des Werkzeugs, nicht nur eine Vorsichtsmaßnahme. Ein
Programm, das Eingaben verändert, wäre als Zeuge über das eigene Fehlerbild nicht
brauchbar. Sie dürfen in Phase 1 unter keinen Umständen aufgeweicht werden:

- Keine DLL-Injection, kein Treiber, kein Code im Spielprozess.
- Kein `SendInput`, `mouse_event`, `SetCursorPos` oder `ClipCursor`. Es werden keine
  Eingaben geschrieben.
- Keine Filterung, Korrektur oder Unterdrückung. Der Hook ruft immer `CallNextHookEx`.
- Kein Tastaturhook. Die drei Hotkeys nutzen `RegisterHotKey` und können daher nur
  genau diese drei Kombinationen sehen. Tastenanschläge und Textinhalte sind technisch
  nicht erfassbar.
- Kein Netzwerkzugriff, keine Adminrechte, kein Installer, keine Registry-Schreibzugriffe.
- Keine Änderung am Spiel oder seinen Dateien.
- Wirkung nur während der Laufzeit, kein Rückstand nach dem Beenden.
- Protokolliert wird ausschließlich für den Zielprozess. Ausnahme sind Fokuswechsel, die
  Fenstergriff, PID und Prozessname des neuen Vordergrundfensters nennen, aber keine
  Eingaben fremder Anwendungen. Diese Ausnahme ist in `docs/diagnostics.md` begründet.

Phasenordnung: Phase 1 erzeugt Belege, Phase 2 entscheidet über Korrekturen. Keine
Korrekturlogik ergänzen, solange kein ausgewerteter Log vorliegt.

## Nicht verhandelbare Implementierungsgrenzen

- Der Hook-Callback darf nichts Blockierendes tun. Windows entfernt einen
  Low-Level-Hook nach `LowLevelHooksTimeout` stillschweigend. Datei-Ein-/Ausgabe,
  Sperren und prozessübergreifende Abfragen gehören in den Writer-Thread.
- Die Ringpuffer-Kopplung ist ein einzelner Produzent und ein einzelner Konsument.
  Weitere Produzenten oder Konsumenten brechen die Annahmen in `EventQueue.h`.
- DPI-Awareness ist eine Korrektheitsbedingung. Der Hook liefert physische Pixel; ohne
  Per-Monitor-Awareness wären alle fensterrelativen Spalten um den Skalierungsfaktor
  falsch.
- Keine externen Abhängigkeiten. Das Werkzeug bleibt eine statisch gelinkte EXE ohne
  Runtime-Redistributable.

## Prüf-Einstiege

```bash
# Cross-Build aus WSL, erzeugt eine native Windows-x64-PE-Datei
cmake -S . -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64-x64.cmake
cmake --build build-mingw -j

# Dokumentationsprüfung
python3 ~/CODING/Homelab/tools/docs/check_docs.py ~/CODING/RemoteMouseFix
```

Unter Windows ist MSVC der bevorzugte Weg, siehe [README.md](README.md).

Ein Build gilt erst als geprüft, wenn er warnungsfrei übersetzt und ein Testlauf einen
Log mit `queue drops 0` erzeugt hat. Für einen Testlauf ohne Spiel genügt ein beliebiger
Zielprozess in `target_process` und `--seconds N`.

## Themenkarte

| Thema | Ort |
| --- | --- |
| Logformat, Spalten, Leseverfahren für einen Klick ins 3D-Sichtfeld | [docs/diagnostics.md](docs/diagnostics.md) |
| Build mit MSVC und Cross-Build, Konfigurationsschlüssel, Testablauf mit TeamViewer | [README.md](README.md) |
| Fehlerbild, Arbeitshypothese und was Phase 1 belegen soll | [README.md](README.md) |
| Messergebnis vom 12.09.2026, Ursachenkette und Randbedingungen für Phase 2 | [docs/findings-2026-09-12.md](docs/findings-2026-09-12.md) |
