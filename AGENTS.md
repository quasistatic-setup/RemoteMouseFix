# RemoteMouseFix: Projektregeln

Globale Regeln in `~/AGENTS.md` gelten zusätzlich und haben bei Git, Geheimnissen und
Dokumentationsform Vorrang vor Gewohnheiten. Diese Datei beschreibt nur, was für dieses
Repository eigen ist.

## Projektgrenzen

Portables Windows-Werkzeug gegen fehlerhafte Maus- und Kamerasprünge in 3D-Spielen über
Fernwartungssoftware. Erster Fall: TeamViewer mit WoW 3.3.5a.

Technisch festgelegt und nicht ohne ausdrücklichen Auftrag zu ändern:
C++17, Win32-API, CMake, Windows 10/11, zunächst x64.

Das Repository enthält ausschließlich Quellcode, Konfigurationsvorlagen, das Testziel
`tools/probe` und Dokumentation. Aufgezeichnete Logs sind Sitzungsdaten und gehören nie
in Git.

## Übergreifende Schutzregeln

In jedem Modus, ohne Ausnahme:

- Keine DLL-Injection, kein Treiber, kein Code im Spielprozess.
- Kein `ClipCursor`, keine Änderung am Spiel oder seinen Dateien.
- Kein Tastaturhook. Hotkeys nutzen `RegisterHotKey` und sehen nur genau diese
  Kombinationen. Tastenanschläge und Textinhalte sind technisch nicht erfassbar.
- Kein Netzwerkzugriff, keine Adminrechte, kein Installer, keine Registry-Schreibzugriffe.
- Wirkung nur während der Laufzeit, kein Rückstand nach dem Beenden.
- Protokolliert wird ausschließlich für den Zielprozess. Ausnahme sind Fokuswechsel mit
  Fenstergriff, PID und Prozessname, aber ohne Eingaben fremder Anwendungen.

Mit `diagnostic_mode = true` wird keine Eingabe verändert und kein Korrekturmodus lässt
sich einschalten. Das ist der Standard der ausgelieferten `config.json`.

Eingriffe in Eingaben sind nur unter allen folgenden Bedingungen zulässig. Jede
Erweiterung darüber hinaus braucht zuerst einen ausgewerteten Log, der sie begründet:

- `diagnostic_mode = false` und ein Korrekturmodus ist aktiv. Der Startmodus ist
  standardmäßig `off`.
- Zurückgehalten werden ausschließlich injizierte `WM_MOUSEMOVE` ohne die eigene
  Signatur in `dwExtraInfo`, und nur solange der Zielprozess im Vordergrund ist und der
  Cursor versteckt ist.
- Tasten, Mausrad und physische Eingaben werden nie zurückgehalten oder verändert.
- Eigene synthetische Eingaben tragen immer die Signatur und werden nie erneut verarbeitet.
- Der Not-Aus-Hotkey `Ctrl+Alt+C` muss registriert sein, sonst bleibt die Korrektur
  gesperrt. Der Nutzer steuert den Rechner womöglich selbst über die Fernwartung und darf
  die Kontrolle über den Zeiger nie verlieren.
- Watchdogs schalten die Korrektur bei Auffälligkeiten ab und schreiben den Grund ins Log:
  zu lange versteckter Cursor, fehlgeschlagene Ausgaben, nicht zurückkommende eigene
  Eingaben.

## Nicht verhandelbare Implementierungsgrenzen

- Der Hook-Callback darf nichts Blockierendes tun. Windows entfernt einen
  Low-Level-Hook nach `LowLevelHooksTimeout` stillschweigend. Datei-Ein-/Ausgabe, Sperren
  und die Korrekturausgabe gehören nicht in den Callback; die Ausgabe wird an die
  Nachrichtenschleife gepostet.
- Die Ringpuffer-Kopplung ist ein einzelner Produzent und ein einzelner Konsument.
- DPI-Awareness ist eine Korrektheitsbedingung. Hook und `SetCursorPos` arbeiten in
  physischen Pixeln.
- Der Warp-Anker des Spiels wird aus den Daten gelernt, nie als Client-Mitte angenommen.
- Keine externen Abhängigkeiten. Jede EXE bleibt statisch gelinkt.

## Prüf-Einstiege

```bash
# Cross-Build aus WSL, erzeugt RemoteMouseFix.exe und MouseLookProbe.exe
cmake -S . -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64-x64.cmake
cmake --build build-mingw -j

# Dokumentationsprüfung
python3 ~/CODING/Homelab/tools/docs/check_docs.py ~/CODING/RemoteMouseFix
```

Unter Windows ist MSVC der bevorzugte Weg, siehe [README.md](README.md).

Ein Build gilt erst als geprüft, wenn er warnungsfrei übersetzt und ein Lauf einen Log
mit `queue drops 0` erzeugt. Eine Änderung an der Korrektur gilt erst als geprüft, wenn
sie gegen `MouseLookProbe` mit injizierten absoluten Eingaben in allen Modi getestet ist.
Testläufe nie im Log-Ordner eines Nutzers ausführen: die Rotation löscht dort Belege.

## Themenkarte

| Thema | Ort |
| --- | --- |
| Logformat, Spalten, Leseverfahren für einen Klick ins 3D-Sichtfeld | [docs/diagnostics.md](docs/diagnostics.md) |
| Build mit MSVC und Cross-Build, Konfigurationsschlüssel, Testabläufe | [README.md](README.md) |
| Fehlerbild, Arbeitshypothese, Korrekturmodi | [README.md](README.md) |
| Messergebnis vom 12.09.2026, Ursachenkette und Randbedingungen für Phase 2 | [docs/findings-2026-09-12.md](docs/findings-2026-09-12.md) |
