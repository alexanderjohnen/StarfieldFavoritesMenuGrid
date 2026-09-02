# Favorites Menu Grid — Arbeitsstand

Stand: 2026-09-02 (siehe Abschnitt 0c). Basisstand der Chronik: 2026-08-30. Basis ist das offizielle **0.7.6** von Nexus, ergänzt um
eigene Features, die es dort nicht gibt.

---

## 0. Stand nach dem Release (2026-09-01)

**Die Mod heißt jetzt `Favorites Menu Grid - SFSE`**, Autor `LX6R`, Version
1.0.0, auf Nexus veröffentlicht als Fork von Sators Favorites Banks
(GPL-3.0-or-later, Quellarchiv liegt daneben). Dateien im Spiel:
`FavoritesMenuGrid.dll`, `FavoritesMenuGrid.ini`, `favoritesmenu.swf`.
Zustandsordner: `Documents / My Games / Starfield / SFSE / Plugins / FavoritesMenuGrid / States`.

**Ausgebaut** wurde alles, was zum Rad gehörte: Mausrad-Seitenwechsel samt
Windows-Hook, XInput/Schultertasten, Seitenanzeiger, `IconSize`,
`IconTelemetry`, `PollIntervalMs`, sämtliche Seitentasten (`Bank1Key`–`Bank8Key`,
`PrevPageKey`, `NextPageKey`, `ModifierKey`) und die Streifen-Seite.

**Umbenannt:** `BankCount` → `RowCount`, `ResetToFirstPageOnClose` → `DefaultRow`
(jetzt Verhalten, keine Option: beim Schließen geht immer diese Zeile in die
echten Slots zurück). Nach außen heißt alles „row", nicht mehr „page" oder
„bank"; im Speicher-Code steht „bank" weiterhin.

**Verbleibende INI-Optionen (8):** `[Grid]` RowCount, DefaultRow,
PowerIconScalePercent · `[Controls]` ClearSlotKey, ToggleEquipOnSelect,
WrapNavigation, ExternallyManagedSlots, PinnedSymbols.

`HideWheel`, `Enabled` und `Icons` sind am 2026-09-02 **ersatzlos entfallen**,
`[Settings]` als Sektion ebenfalls (0h).

Alles Ältere in diesem Dokument ist Chronik: die beschriebenen Fehler und
Ursachen stimmen, die genannten Optionen gibt es teils nicht mehr.

## 0b. 1.0.1 — gebaut und deployed, ungetestet (2026-09-02)

Ausgelöst durch den ersten Nexus-Kommentar (fabs1): Favoritenmenü „filled but
visually empty without icons", und ein alter Spielstand zeigte die Favoriten
des zuletzt gespielten. Beides war dieselbe Wurzel.

**1. Kein Erben mehr zwischen Spielständen** (`LoadBestStateLocked`,
`favorites_core.cpp`). Neu ist die Unterscheidung, ob der eingehende Spielstand
*benannt* werden konnte: `g_incomingSaveName` kommt aus
`queuedEntryToLoad->GetFileName()` und ist der Dateiname, den der Spieler
gewählt hat; `ReadMostRecentSaveName()` ist dagegen nur der neueste Stand auf
der Platte, also beim Laden eines älteren immer falsch. Ist der Name bekannt
und existiert kein Schnappschuss, bricht der Ladepfad dort ab — **kein
Rückfall auf `current.fbs`, keine Legacy-Pfade**. Genau dieser Rückfall hat die
Reihen eines fremden Spielstands hereingetragen: Der Zustand liegt neben dem
Save, spult also nicht mit ihm zurück.

**2. Übernahme auf die Standardreihe** (`InitializeSessionIfNeeded`, else-Zweig).
Vorher fest `g_banks[0]`, jetzt `g_settings.defaultRow - 1`. Das ist die Reihe,
die beim Schließen ohnehin in die echten Slots zurückgeht, also die, auf die
die Quickkeys zeigen.

**3. Label-Rückfall pro Zelle** (`favorites_grid.cpp`, `drawIcon`). Ein belegter
Slot ohne `visual.imageName` zeichnete im Icon-Zweig **gar nichts** — belegt,
anklickbar, sichtbar leer. `ShortLabel` fällt längst auf die Editor-ID zurück,
war aber nur bei `Icons=0` erreichbar. Jetzt entscheidet das jede Zelle für
sich. README und Nexus-Beschreibung behaupteten dieses Verhalten schon vorher.

Drei neue Tests sichern genau diese drei Invarianten (40 gesamt, grün).
README, `docs/NEXUS_DESCRIPTION_BBCODE.txt` und
`docs/NEXUS_UPLOAD_FIELDS_EN.txt` sind angepasst, `xmake.lua` steht auf 1.0.1.

**Offen:** Alexander hat noch nicht getestet. Der aussagekräftige Durchgang ist
ein Spielstand von *vor* der Installation: Landen die zwölf Favoriten auf
Reihe 1, sind die übrigen Reihen leer, zeigen Zellen ohne Icon ihren Namen?
Danach erst ein Archiv schnüren — Archive gelten ab Übergabe als ausgeliefert.

**Verworfen unterwegs:** ein INI-Schalter `ClearFavoritesOnFirstRun`. Unnötig,
sobald „Erstkontakt" sauber von „Erkennung fehlgeschlagen" getrennt ist. Ebenso
verworfen: Namen und Icons direkt aus dem Item lesen statt aus der Menükarte —
im Prinzip richtig, würde aber erst Reverse Engineering brauchen (wo Starfield
den Bildnamen ablegt), und das war als „überschaubar" zu vollmundig behauptet.

**Erledigt 2026-09-02:** Das Nexus-Thumbnail ist ausgerichtet. Quelle war
`Downloads/Screenshot 2026-09-01 211039.png` (1080x607), Motiv y=146..542, also
146 px Rand oben gegen 64 px unten. Oben 41 px abgeschnitten, unten 41 px
angesetzt: Ränder 105/105, Maße unverändert.

**Fallstrick beim Messen:** Die Motiv-Oberkante ist die *hellgraue* Tooltip-
Leiste bei y=146, nicht die dunkle Schrift darin bei y=216. Ein erster Versuch
suchte Pixel mit Luminanz > 200 und übersah den Kasten deshalb komplett — 76 px
statt 41 gekappt, und unten war sichtbar mehr Luft als oben. Verlässlich ist
stattdessen ein Kantenmaß: je Zeile zählen, wie oft benachbarte Pixel um mehr
als 18 Luminanzstufen springen. Der weiche Hintergrundverlauf tut das nie,
jede Kante des Motivs schon — unabhängig davon, ob sie hell auf dunkel liegt
oder umgekehrt. Die
neuen Zeilen sind kein Klon der letzten Zeile, sondern das saubere
Hintergrundband y=555..606 vertikal auf 128 px gedehnt — bei einem weichen
Verlauf unsichtbar, und der Schattensaum unter der Schrift (y=543..554) bleibt
1:1 erhalten, sonst entstünde direkt unter den Buchstaben eine harte Kante.
Ergebnis: `Downloads/FavoritesMenuGrid-thumbnail.png`. Gerechnet mit
System.Drawing über PowerShell, weil kein Pillow installiert ist.

## 0c. 1.0.1 im Spiel geprueft — drei Fehler gefunden und behoben (2026-09-02)

Alexander hat getestet. 1.0.1 war in der ersten Fassung eine Verschlechterung.

**1. Der Ladepfad verwarf den Zustand des eigenen Spielstands.** Das Log zeigte
`is loading save 'Save435_..._20260901175833_49_2_4'` und direkt danach
`No stored pages for save 'Save435_...'`. In `current.fbs` stand aber
`SAVE_NAME "Save435_..._20260901175833_49_2_4"` — derselbe Name. Die Datei
gehoerte zu genau diesem Spielstand und wurde trotzdem weggeworfen.

Ursache war eine Asymmetrie: Snapshots (`<savename>.fbs`) entstehen **nur** bei
`kSaveCompleted`, der Ladepfad akzeptierte aber **nur noch** Snapshots. Im
States-Ordner lag keine einzige Snapshot-Datei. Damit startete jeder
Spielstand leer, der vor diesem Mechanismus entstanden ist — also jeder
Spielstand jedes Nutzers.

Behoben in `LoadBestStateLocked`: `current.fbs` gilt wieder als gueltige
Quelle, **wenn sein `SAVE_NAME` dem eingehenden Spielstand entspricht**. Der
Stempel unterscheidet die beiden Faelle, die vorher nicht zu trennen waren:
gleicher Name = eigener Zustand ohne Snapshot, anderer Name = fabs1s Leck.
Bei Nichtuebereinstimmung muss `g_banks` zurueckgesetzt werden — `ReadStateFile`
hat die Reihen zu dem Zeitpunkt schon eingetragen.

**2. Der Stempel selbst war unzuverlaessig.** `SaveCurrentStateLocked` nahm fuer
`current.fbs` immer `ReadMostRecentSaveName()`, also den neuesten Stand auf der
Platte. Das stimmt nur in dem Moment, in dem das Spiel gerade gespeichert hat.
Laedt man einen aelteren Stand und spielt ohne zu speichern, benennt es einen
fremden Spielstand — und Korrektur 1 haette diesen Pages genau dorthin
gegeben. Jetzt: bei `saveSnapshot` weiterhin `recent` (das Spiel hat eben
geschrieben), sonst `g_loadedSaveName`. Der Snapshot heisst weiter nach
`recent`, nicht nach dem Stempel.

**3. Eine Karte ohne Icon loeschte das gespeicherte Icon.** Alexanders Fund: zwei
Turret-Slots blieben ohne Symbol, obwohl sie vorher eins hatten. `HarvestMenuVisuals`
schrieb `slot.visual = harvested[index]` komplett. Immer wenn das Rad eine Karte
mit `sName`, aber ohne `iconImage` liefert, ist `HasData()` wegen des Namens
wahr, also ersetzte diese Karte das gute Visual — und die Ernte schreibt Zustand, der Verlust war also
einseitig und dauerhaft. In `current.fbs` stand danach
`VISUAL 0 11 1 0 0 0 0 -1 "Technician's Deployable Turret" "" "" ""`:
`fixtureType -1`, Verzeichnis und Bildname leer. Jetzt wird gemischt statt
ersetzt: bringt die Karte keine Icon-Felder, bleiben die gespeicherten stehen.
Der Vergleich „unveraendert, nichts zu tun" muss gegen das **gemischte**
Ergebnis laufen, sonst wird ein zurueckgeholtes Icon wegverglichen.

**Wodurch die Karte icon-los wurde, ist offen.** Alexanders eigener Verdacht:
Er hat das Turret im Edit-Modus mit dem Nachbar-Slot getauscht, und das Script
hat es auf den alten Slot zurueckgezwungen. Dass danach **zwei** Zellen
„Deployable Turret" ohne Icon zeigten, passt dazu deutlich besser als die
zuerst vermutete Erklaerung „Item gerade nicht dabei". Der Loesch-Mechanismus
oben ist davon unabhaengig richtig — die leeren Icon-Felder in `current.fbs`
belegen ihn —, aber der **Ausloeser** gehoert noch untersucht: Was macht ein
zurueckgezwungener Tausch mit den Visuals der beteiligten Slots, und warum
entsteht dabei ein Duplikat? Reproduzieren: Slot im Edit-Modus mit dem
Nachbarn tauschen, Rad schliessen, `current.fbs` ansehen.

Bereits verlorene Icons kommen wieder, sobald das Rad einmal geoeffnet wird,
waehrend das Item getragen wird.

**Nicht behoben, weil kein Fehler:** die fehlenden Icons in den ersten beiden
Testbildern. Das war Folge von 1 — ohne geladenen Zustand baut die Uebernahme
die Reihe aus den nativen Slots, und die tragen keine Karteninformation. Der
Editor-ID-Rueckfall aus 1.0.1 griff korrekt und machte nur sichtbar, dass
nichts da war.

42 Tests gruen. Der Test, der das alte Verhalten festschrieb
(`test_a_save_the_plugin_has_named_never_inherits_another_saves_pages`), war
selbst falsch und schreibt jetzt die Namensgleichheit fest; zwei neue Tests
sichern Stempel und Icon-Erhalt. **Am Geraet bestaetigt (2026-09-02, 01:32 und 01:37).** Beide Zweige von
Korrektur 1 greifen:

```
01:32 loading save 'Save435_...'
01:32 state loaded from '...current.fbs': 4 stored bank(s)
01:37 loading save 'Save89_...'
01:37 No stored pages for save 'Save89_...' (the shared state belongs to
      'Save435_...'); ... source='new state'
```

Vorher stand an beiden Stellen `No stored pages`. Der Ablehnungspfad mit dem
`g_banks`-Reset laeuft also auch: Save89 startete sauber leer, ohne Save435s
Reihen zu erben. Korrektur 3 ist noch ungeprueft.

**Warum nach der Wiederherstellung manche Zellen Icons hatten und manche
nicht** (Reihe 1 gemischt, Reihen 2-4 vollstaendig): Der von Hand
zurueckgespielte Zustand stammte aus dem Ordner vor der Umbenennung und passte
nicht mehr zu dem, was der Spielstand in den nativen Slots haelt. Der Log sagt
es: `Loaded native page did not exactly match any stored bank; reconciling it
with bank 1`. Nur Reihe 1 wird gegen die nativen Slots abgeglichen, und der
Spielstand ist dabei die Autoritaet — Items, die er haelt und die der alte
Zustand an dieser Stelle nicht kannte, kommen ohne Visual herein und zeigen
ihre Editor-ID. Reihen 2-4 fasst der Abgleich nicht an, deshalb sind sie
durchgehend bebildert. Kein Fehler, sondern Folge der Handarbeit.

**Messfallstrick fuer die naechste Runde:** Erst ins Log schauen
(`Documents/My Games/Starfield/SFSE/Logs/Favorites Menu Grid.log`) und in
`current.fbs` — beide zusammen haben diesen Fehler in Minuten erklaert, wo
Codelesen allein im Kreis gefuehrt haette.

---

## 0d. Veraltete Kartendaten: Icons und Stueckzahl (2026-09-02)

Dieser Abschnitt deckt **drei Meldungen mit einer Ursache**: fabs1s
„filled but visually empty without icons", ZombieMonkeyNZs eingefrorene
Stueckzahl, und Alexanders Beobachtung, dass Icons erst nach dem Edit-Modus
erscheinen.

**Gemeinsame Wurzel:** `slot.visual` ist eine Kopie der Karte, die das Rad
liefert, und `HarvestMenuVisuals` ist der einzige Kanal, der sie auffrischt.
Mit `HideWheel=1` stehen alle zwoelf `Entry_N` auf `visible=false` (steht so
im Log), die Karten werden also nicht neu befuellt — und damit veraltet
alles, was aus ihnen stammt.

**Stueckzahl: behoben (2026-09-02).** Belegt von Alexander: zwei Granaten
geworfen, Anzeige blieb bei `IMPACT GRENADE (59)`, nach einem Item-Tausch
stand korrekt `(57)` da. Die Zahl geht ueber `BuildFavoriteInfoForDisplay` →
`uCount` aus `visual.count` in das **vanilla** `ItemInfo_mc`. Das Panel ist
also vanilla, nur der Inhalt ist unsere veraltete Kopie.

Die Zahl braucht die Karte gar nicht: `BGSInventoryItem::stacks[].count`
traegt sie, ein schlichter Datenzugriff ohne Engine-Aufruf. `NativePage`
sammelt jetzt `carriedCounts` im selben Inventardurchlauf, der ohnehin
`carriedForms` fuellt, und `RefreshCarriedFlagsLocked` schreibt sie in
`descriptor.visual.count`. Das laeuft bei jedem Oeffnen des Menues und bei
jeder Erfassung — also genau dann, wenn die Zahl gebraucht wird.

**Icons: weiter offen.** Fuer sie gibt es keine solche Abkuerzung; wo
Starfield den Bildnamen ablegt, muesste erst gefunden werden (siehe die
verworfene Ueberlegung in 0b). Der naheliegende Weg bleibt herauszufinden,
was der Edit-Modus mit den `Entry_N`-Clips macht, das das Befuellen der
Karten ausloest — ernten, bevor das Rad versteckt wird, oder dieselbe
Aktualisierung beim Oeffnen anstossen.

**Sobald das steht:** ZombieMonkeyNZ antworten. Sein Verdacht auf StarUI und
Free Lanes Update ist unbegruendet, das ist rein hausgemacht.

Der wichtigste Fund des Testabends, von Alexander an einem frisch gesehenen
Spielstand belegt: Reihe 1 zeigt **ohne** Edit-Modus nur Editor-IDs
(`GRENDEL`, `NOVABLAST`, dreimal `SPACESUIT_CONSTELLATI.`, `WT_TORTLEAID`,
`CUTTER`). Sobald `EDIT: ON` gesetzt ist, stehen in **denselben Zellen die
richtigen Icons** — Gewehr, Gewehr, Anzug, Rucksack, Helm, Medipack. Nichts
wurde dazwischen bearbeitet.

Die Icon-Daten sind also erreichbar, sie kommen nur nicht an, solange das Rad
nicht im Edit-Modus ist. `HarvestMenuVisuals` liest die Karten des Rads
(`iconImage` → `sDirectory`/`sImageName`); mit `HideWheel=1` sind die
`Entry_N`-Clips beim Oeffnen unsichtbar, und der Log bestaetigt das:
`child[3] 'Entry_6' visible=false` fuer alle zwoelf. Der Verdacht ist, dass
das Spiel die Karten erst befuellt, wenn das Rad sichtbar wird, und dass der
Edit-Modus genau diese Befuellung ausloest.

**Das ist sehr wahrscheinlich fabs1s eigentliche Beschwerde** („filled but
visually empty without icons"). Die drei Korrekturen in 0c erklaeren seinen
Bericht nur zur Haelfte; dieser Punkt erklaert ihn ganz.

**Naechster Schritt:** herausfinden, was der Edit-Modus anders macht — ernten,
bevor das Rad versteckt wird, oder dieselbe Aktualisierung beim Oeffnen
ausloesen. Vorher nicht als geloest melden.

---

## 0e. Snapshots werden nie geschrieben — offen, blockiert 0c (2026-09-02)

Alexanders zweiter Testdurchgang (Favoriten neu setzen, neu speichern als
Save436, alten Stand laden, Save436 laden) ist gescheitert: Reihen 2-4 leer,
Reihe 1 nur Editor-IDs.

**Der Beweis ist eine fehlende Zeile.** Im gesamten Log:
`wrote per-save state` = **0 Treffer**, `could not write a per-save state
file` = **0 Treffer**. Beide Zweige des Snapshot-Schreibens sind nie gelaufen,
obwohl gespeichert wurde. Im States-Ordner liegt nach wie vor ausschliesslich
`current.fbs` — es hat noch **nie** einen Snapshot gegeben.

Damit haengt der ganze Entwurf aus 0c in der Luft. `current.fbs` traegt immer
den zuletzt gespielten Spielstand, also lehnt jeder Ladevorgang sie ab:

```
01:42 Save435 -> shared state belongs to Save89   -> new state
01:50 Save89  -> shared state belongs to Save435  -> new state
01:51 Save436 -> shared state belongs to Save89   -> new state
```

Die Ablehnung ist jedes Mal formal richtig. Der Fehler liegt davor: Es
entsteht nie etwas, das angenommen werden koennte.

**Verdacht, ungeprueft:** `RE::SaveLoadEvent::Status::kSaveCompleted` (= 4 in
`Events.h`) wird vom Spiel nicht geliefert. Der Enum-Wert existiert, das sagt
aber nichts darueber, ob das Ereignis ankommt — siehe die Regel zu
CommonLibSF-IDs. **Nicht darum herumbauen, bevor das belegt ist.**

**Diagnose ist eingebaut und deployed:** `main.cpp` protokolliert jetzt jedes
`SaveLoadEvent` mit `opType` und `status`. Ein einziger Speichervorgang
beantwortet die Frage. Erwartet waere beim manuellen Speichern
`opType=5 (kManualSave)` und danach `status=4 (kSaveCompleted)`. Bleibt
`status=4` aus, muss der Ausloeser ein anderer sein — der naechstliegende
Kandidat ohne neues Ereignis ist, `mostRecentSaveGame` beim naechsten
Erfassungspunkt mit `g_loadedSaveName` zu vergleichen: Weicht es ab, hat der
Spieler seither gespeichert. Das ist ein reiner Datenlesevorgang, kein
Engine-Aufruf.

**Nebenbefund, offen:** Alexander hat das Technician-Set angezogen, die Items
zugewiesen, das Set wieder ausgezogen — die Slots waren korrekt ausgegraut.
Nach dem Speichern und erneutem Oeffnen waren sie **nicht mehr** ausgegraut,
ohne dass er die Items wieder getragen haette. `RefreshCarriedFlags` meldet
also faelschlich „getragen". Noch nicht untersucht.

---

## 0f. 1.0.2: Die Per-Save-Datei ist jetzt der Hauptspeicher (2026-09-02)

**Korrektur zur Versionslage:** Auf Nexus steht **1.0.1**, und das ist die
1.0.0 mit korrigierter README — nicht der Umbau aus 0b/0c. Es war also nie
etwas Kaputtes ausgeliefert. Gearbeitet wird an **1.0.2**.

**Der eigentliche Konstruktionsfehler** (aus 0e): Die Per-Save-Datei hing
allein an `RE::SaveLoadEvent::Status::kSaveCompleted`. Damit trug ein
Ereignis, dessen Zustellung sich nicht ueberpruefen laesst, den ganzen
Entwurf — und es kam nie an. Ergebnis: kein einziger Snapshot, jeder
Ladevorgang fiel auf die geteilte Datei zurueck, und die nannte inzwischen
einen anderen Spielstand.

**Umgestellt:** Die Per-Save-Datei wird jetzt an **jedem** Erfassungspunkt
geschrieben (sieben Stellen, u.a. Radschliessen), nicht nur nach einem
Speichervorgang. Sie ist der Hauptspeicher; `current.fbs` ist nur noch der
Rueckfall fuer Installationen, die sie noch nicht haben. `saveSnapshot` ist
damit bedeutungslos geworden und wird nur noch entgegengenommen.

**Wie ein neuer Spielstand ohne Ereignis bemerkt wird:**
`mostRecentSaveGame` aendert sich ausschliesslich, wenn das Spiel einen Stand
schreibt. Beim Sitzungsbeginn wird der Wert in `g_recentSaveAtSessionStart`
festgehalten; weicht er spaeter ab, hat der Spieler seither gespeichert, und
dieser Stand ist der, zu dem die Reihen jetzt gehoeren. **Wichtig:** verglichen
wird gegen die Startaufnahme, **nicht** gegen `g_loadedSaveName` — sonst
schlaegt es sofort an, wenn ein aelterer Stand geladen wird, wo der neueste
Stand auf der Platte natuerlich ein anderer ist. Genau diese Verwechslung war
der Fehler in 0c, Korrektur 2.

Die Diagnose aus 0e bleibt vorerst drin: `main.cpp` protokolliert jedes
`SaveLoadEvent` mit `opType` und `status`. Sie ist jetzt aber nur noch
Erkenntnisgewinn, kein Bauteil — falls `kSaveCompleted` doch geliefert wird,
schreibt es den Snapshot einen Moment frueher, mehr nicht.

**Erster Erfolg am Geraet (2026-09-02, 19:47).** Zum ersten Mal in der
Projektgeschichte existiert eine Per-Save-Datei:

```
19:47:36 wrote per-save state for 'Autosave1_..._20260901175409_49_2_4'
```

2906 Bytes, neben `current.fbs` im States-Ordner. Der tragende Teil ist
repariert.

**Dabei sofort ein eigener Fehler aufgefallen:** Zeile 54 meldete
`noticed a new save 'Autosave1_...'`, obwohl nichts gespeichert wurde — es ist
derselbe Stand, der geladen wurde. Die Annahme, `mostRecentSaveGame` aendere
sich nur beim *Schreiben*, ist falsch: **es aendert sich auch beim Laden**,
und beim Lesen in `LoadBestStateLocked` steht es noch auf dem vorigen Wert.
Der erste Erfassungspunkt danach sah also eine Abweichung, die keine war.
Folgenlos (der gesetzte Name war ohnehin der richtige), aber die Aufnahme wird
jetzt **beim ersten Erfassungspunkt** scharf gestellt statt beim Laden: ist
`g_recentSaveAtSessionStart` leer, wird der Wert nur uebernommen, nicht
gemeldet.

**Die Diagnose hat 0e beantwortet — gemessen, nicht geschlossen.** Ein
Quicksave erzeugt:

```
19:50:25 SaveLoadEvent: opType=3 status=0     kQuicksave, kBegin
19:50:25 SaveLoadEvent: opType=3 status=1     kQuicksave, "kLoadSucceeded"
```

**`status=4` (`kSaveCompleted`) kommt nie.** Ein abgeschlossener
Speichervorgang meldet sich als `status=1` — der Wert, den `Events.h`
`kLoadSucceeded` nennt. Der Name traegt nicht: 1 heisst „die Operation ist
geglueckt", fuer Laden **und** Speichern. Nur der `opType` unterscheidet die
beiden.

Damit war nicht nur der Snapshot-Zweig tot. Der bestehende Code behandelte
`status=1` unbesehen als Ladevorgang und startete nach **jedem Quicksave**
die Sitzungsinitialisierung neu.

**Behoben:** `IsSaveOperation` (kAutosave, kQuicksave, kManualSave,
kExitSaveToMainMenu, kExitSaveToDesktop) entscheidet jetzt bei `status=1`,
ob erfasst oder initialisiert wird. Besonders wertvoll sind dabei die
Exit-Save-Operationen: Sie sind die letzte Gelegenheit, Zustand zu sichern,
bevor das Spiel schliesst — genau die Luecke, durch die Alexanders Quicksave
um 19:50 keinen Snapshot mehr bekam, weil danach kein Erfassungspunkt mehr kam.

Der `kSaveCompleted`-Zweig bleibt stehen: Falls er je geliefert wird, ist die
Reaktion richtig. Die Diagnosezeile bleibt vorerst auch drin.

**Merksatz fuer CommonLibSF:** Ein Enum-Wert im Header ist kein Beleg, dass
die Engine ihn jemals sendet — und ein Enum-*Name* ist kein Beleg dafuer, was
der Wert bedeutet. Beides hat hier zugeschlagen.

44 Tests gruen. Gebaut und deployed als 1.0.2.

**Test dafuer:** Reihen setzen, Rad schliessen — im Log muss jetzt
`wrote per-save state for '...'` stehen, und im States-Ordner muss eine
`<Spielstandsname>.fbs` liegen. Das ist die Zeile und die Datei, die es in
der gesamten Historie noch nie gegeben hat. Erst danach lohnt der Wechsel
zwischen Spielstaenden.

---

## 0g. Rundlauf bestaetigt — 0e ist erledigt (2026-09-02, 20:06)

Alexanders Durchgang: Reihen setzen, quickspeichern, alten Stand laden,
zurueck auf den neuen. **Alle vier Reihen samt Icons wieder da.** Die Zeile,
die es nie gab:

```
20:06:01 state loaded from '...\Save436_..._20260902180449_49_2_4.fbs':
         4 stored bank(s)
```

Geladen aus der Per-Save-Datei mit Spielstandsnamen, nicht aus `current.fbs`.
Im States-Ordner liegen sechs eigene Zustandsdateien statt keiner, auch fuer
den alten Save89 (der beim ersten Kontakt sauber leer startete und dabei
seinen eigenen Snapshot bekam — beim naechsten Laden ist er kein Erstkontakt
mehr). Der Quicksave-Zweig greift ebenfalls: `opType=3 status=1` →
`wrote per-save state for 'Save435...'`.

**Nachgezogen, Kosmetik:** `noticed a new save` meldete sich auch nach einem
*Laden*. `mostRecentSaveGame` wandert einige Sekunden nach dem Laden auf den
geladenen Stand — das ist kein neuer Spielstand. Zusaetzliche Bedingung:
`recent != g_loadedSaveName`. Beide Bedingungen werden gebraucht; die
Startaufnahme allein wuerde in dem Fenster, bevor der Wert sich setzt, den
neuesten Stand auf der Platte uebernehmen, und das ist genau falsch, wenn
absichtlich ein aelterer geladen wurde.

**Der Ausgegraut-Befund, zweiter Anlauf.** Der erste Fix war noetig, aber
nicht die ganze Ursache. Alexanders praezise Beschreibung: Beim Ausziehen des
Technician-Sets verschwinden **Granate, Field Kit und Turret** aus dem
Inventar (nicht die Anzugteile selbst). Dann sind die Zellen korrekt blass —
bis gespeichert wird, danach sind sie wieder hell, und auch nach dem Laden
des Spielstands hell.

**Ursache: ein Feld, zwei Bedeutungen.** `descriptor.unresolved` wird von
zwei Stellen geschrieben, die verschiedene Fragen beantworten:

- `RefreshCarriedFlags` fragt „liegt das Item im Inventar".
- Der Commit beim Radschliessen fragt „konnte ich es in einen echten Slot
  schreiben" und setzte `descriptor.unresolved = !success`.

Die beiden widersprechen sich genau dann, wenn ein Item das Inventar
verlassen hat, sein Favorit aber weiterhin zuweisbar ist — dann **loeschte**
der Commit die Markierung. Deshalb wurde eine zu Recht blasse Zelle hell,
sobald das Rad zuging, und das sah aus, als haette das Speichern es getan.
Nach dem Laden dasselbe ueber den Materialisierungspfad.

**Behoben:** Der Commit **hebt** die Markierung nur noch bei Fehlschlag und
senkt sie nie; das Senken gehoert allein der Trageprüfung. Zusaetzlich zieht
der Commit `DescriptorItemIsCarried` gegen den frisch erfassten
`NativePage` nach, damit die geschriebene Seite direkt stimmt.
`unresolved` wird bewusst **nicht** in die Zustandsdatei geschrieben: Es ist
abgeleiteter Zustand und wird nach jedem Laden neu berechnet.

**Der erste Teilfix bleibt richtig und noetig:** `descriptor.unresolved` wurde nur
von `RefreshCarriedFlags` richtig gesetzt, und die hatte genau **eine**
Aufrufstelle: nach dem Laden. Jede Erfassung dazwischen laeuft ueber
`ReconcileNativePageWithBank`, und die liest die nativen Slots — die halten
das Item weiter, auch wenn der Spieler es nicht mehr traegt. Also wurde die
Blass-Markierung bei jeder Erfassung geloescht, und ein Slot wurde wieder hell,
nur weil gespeichert wurde. Der Kern ist jetzt als `RefreshCarriedFlagsLocked`
herausgezogen und laeuft in `CaptureCurrentBankLocked` **nach** dem Reconcile,
der sie loescht.

45 Tests gruen. Gebaut und deployed. **Offen bleibt aus 0d nur noch das Icon.**

---

## 0h. Optionen ausgemistet, INI neu gegliedert (2026-09-02)

Alexanders Entscheidung, und sie ist richtig: Es gab keine Einstellung dieser
Option, die Sinn ergab. `HideWheel=0` liess Rad und Raster **uebereinander**
liegen — das stand sogar so im INI-Kommentar. Das Raster existiert genau
dafuer, das Rad nicht mehr ansehen zu muessen. Ein Problem mit dem Verstecken
waere ein Problem zum Beheben, kein Schalter fuer den Spieler.

Entfernt aus `favorites.h` (`gridHidesWheel`), beiden Nutzungen in
`favorites_grid.cpp` (Verstecken und Positionieren von `ItemInfo_mc` laufen
jetzt bedingungslos), dem INI-Lesen und der Startzeile in `main.cpp`, sowie
aus der Referenz-INI. **Auch aus der deployten INI im Spielordner** —
`deploy.py` fuegt nur hinzu und entfernt nie, ein toter Schluessel waere also
stehengeblieben.

### Und `[Grid] Enabled` gleich mit

Alexanders naechste Frage — was bliebe eigentlich uebrig, wenn das Raster aus
ist? — liess sich nachpruefen, und die Antwort ist: nichts Zusammenhaengendes.

- Es gibt **keinen Weg, die Reihen 2-4 zu erreichen**. Saemtliche Seitentasten,
  das Mausrad und die Streifen-Seite sind beim Release ausgebaut worden. Der
  einzige verbliebene Aufrufer von `QueueBankSwitch` war
  `BankSelectorClickHandler` in `favorites_ui.cpp` — und der war **definiert,
  aber nirgends registriert**. Toter Code aus dem entfernten Streifen, jetzt
  entfernt.
- `ClearSlotKey` war ohnehin an `showGrid` gehaengt, funktionierte ohne Raster
  also gar nicht.
- Uebrig blieben `ToggleEquipOnSelect` und `ExternallyManagedSlots` plus eine
  Buchfuehrung ueber Reihen, die niemand sehen kann.

Das ist kein Modus, das ist ein Defekt mit Schalter. `Enabled` ist deshalb
ebenfalls raus: `showGrid` aus `favorites.h`, beide Abfragen in
`favorites_grid.cpp` (nur die Konfliktsperre `g_disabledForConflict` bleibt,
die ist etwas anderes), INI-Lesen und Startzeile in `main.cpp`, die Kopplung
der Loeschtaste, und beide INIs.

### Und `Icons` als dritte

Begruendet war sie im Test mit „a player whose engine chokes on the streaming
has to be able to fall back to names" — also eine Notluke gegen Abstuerze, und
die Mod hat dazu Vorgeschichte (Icon-Crash, Asset-Streaming bei parallelen
Zeichendurchlaeufen). Trotzdem raus, aus zwei Gruenden: Der Rueckfall greift
laengst **pro Zelle** (kein Bildname → Name), und die genannten Abstuerze sind
behobene Fehler. Eine Option „falls unser Code abstuerzt" ist dieselbe Sorte
wie `HideWheel`.

Beim Ausbau ist der `else`-Zweig des reservierten Streifens aufgefallen, der
an derselben Bedingung hing: Er bekommt jetzt mit `drawPinIcon` dieselbe
Pro-Zelle-Regel wie eine Rasterzelle — vorher gab es sie dort gar nicht.

### INI neu gegliedert — und die Falle dabei

Alexanders Einwand: `[Settings]` ist ein Sammelbecken ohne Bedeutung. Neue
Gliederung, zwei Sektionen:

- **`[Grid]`** — RowCount, DefaultRow, Icons, PowerIconScalePercent.
  `DefaultRow` steht bewusst neben `RowCount`: „wie viele Reihen und welche
  ist die Standardreihe" gehoert zusammen, auch wenn man `DefaultRow` mit
  etwas gutem Willen als Steuerungsoption lesen kann.
- **`[Controls]`** — ClearSlotKey, ToggleEquipOnSelect, ExternallyManagedSlots,
  PinnedSymbols. `PinnedSymbols` steht wie gewuenscht direkt hinter
  `ExternallyManagedSlots`, weil es nur reservierte Slots betrifft.

**Die Falle, und sie ist ernst:** `GetPrivateProfileInt` kann „Schluessel
fehlt" nicht von „Wert ist zufaellig der Standard" unterscheiden. Ein
Schluessel, der die Sektion wechselt, setzt sich damit fuer **jeden
bestehenden Nutzer still auf den Standard zurueck** — dieselbe Klasse Fehler,
die schon einmal `ExternallyManagedSlots` lahmgelegt hat, als beim Ausbau der
Hotkey-Buttons der umgebende Leseblock mitverschwand.

Deshalb `ReadMovedInt`: liest mit dem Sentinel `0x7FFFFFFF`, faellt bei
Abwesenheit auf die alte Sektion zurueck und protokolliert das. Betrifft
RowCount, DefaultRow (frueher `[Settings]`), ToggleEquipOnSelect (frueher
`[Settings]`) und PinnedSymbols (frueher `[Grid]`).

`deploy.py` fuegt nur hinzu und entfernt nie, deshalb wurde Alexanders
deployte INI direkt neu geschrieben — mit Uebernahme seiner Werte, unter
anderem `ExternallyManagedSlots=0`.

Nebeneffekt fuer den Controller: Die Frage, ob die neue Steuerkreuz-Belegung
an einer Option haengen muss, entfaellt damit vollstaendig. Es gibt keine
Bedingung mehr — die Mod ist das Raster.

---

## 0i. Auf GitHub, und ein gejagtes Gespenst (2026-09-02)

**Das Projekt liegt jetzt unter Versionskontrolle und oeffentlich auf GitHub:**
`https://github.com/alexanderjohnen/StarfieldFavoritesMenuGrid` (GPL-3.0, wie
`StarfieldVATS`). Erster Commit `01fbdb7`. Vorher hatte dieses Projekt
**ueberhaupt keine Historie** — und genau das hat am selben Abend Geld
gekostet, siehe unten. Ab jetzt: vor jedem Deploy committen.

**Das Gespenst:** Nach einem Dutzend Aenderungen lud ein Spielstand nicht mehr
— erst ein Hardcrash, dann Schwarzbild ohne Ende. Der Log endete direkt nach
`is loading save`, also mitten in `BeginLoadTransition()`. Ich habe daraufhin
zwei Mechanismen im eigenen Code konstruiert (eine verbreiterte
Sperrreihenfolge im Commit, die `stacks`-Schleife in `CaptureNativePage`) und
Diagnose dafuer eingebaut.

**Es war nichts davon.** Der Crashlog zeigte eine Zugriffsverletzung in
`SpecialK64.dll` auf `BSJobs 1`, ohne unsere DLL im Backtrace, und unser Log
zeigte, dass das Plugin zum Absturzzeitpunkt seit einer Minute nichts getan
hatte. Alexander hat den PC neu gestartet — seither laedt alles. Vorausgegangen
war ein erzwungenes Beenden von Starfield ueber den Task-Manager.

**Lehre, teurer als sie klingt:** Ein Fehlerbild, das nach vielen Aenderungen
auftaucht, verfuehrt dazu, die Ursache unter den eigenen Aenderungen zu suchen.
Der billigste Test ist der, den ich zu spaet vorgeschlagen habe — **einmal ohne
die eigene DLL starten**. Und der zweitbilligste ist eine Historie, mit der
sich zurueckspringen laesst, statt Hypothesen zu bauen. Beides gibt es jetzt.

Die Diagnosemarken fuer diesen Phantomfehler (`load-transition:`, `capture:`)
sind wieder entfernt. `carried-probe`, `draw-probe` und `SaveLoadEvent:`
bleiben vorerst, bis das Ausgegraute geklaert ist — **alle drei muessen vor
der Auslieferung von 1.0.2 raus.**

---

## 0a. Controller-Support: Entwurf, ungebaut

Wird auf Nexus und Discord nachgefragt. Der Weg ist recherchiert.

**Die Praemisse hat sich am 2026-09-02 geaendert:** Alexander hat einen
Xbox-Controller hervorgeholt und testet selbst. „Gesucht wird ein Tester"
gilt fuer die *Entwicklung* nicht mehr — bestenfalls noch fuer die
Bestaetigung auf fremder Hardware (andere Controller, Steam-Input-Remapping).

Das ist wichtiger, als es klingt: Praktisch jeder belastbare Befund dieses
Projekttags kam aus dem Log und aus den Zustandsdateien, nicht aus dem
Nachdenken ueber Code. Drei Hypothesen lagen daneben und wurden von
Artefakten widerlegt. Bei einem entfernten Tester kostet jede falsche
Hypothese einen vollen Umlauf statt zwei Minuten. **Wenn doch einmal aus der
Ferne getestet wird: nicht nach Beschreibungen fragen, sondern nach
`Documents/My Games/Starfield/SFSE/Logs/Favorites Menu Grid.log` — und vorher
eine Diagnose einbauen, die die Frage in *einem* Durchlauf beantwortet.**
Genau so haben `SaveLoadEvent: opType/status` und `carried-probe`
funktioniert.

**Der entscheidende Fund** steht in `swf-src/scripts/FavoritesMenu.as`:

```actionscript
public function get selectedIndex() : uint
public function set selectedIndex(param1:uint) : void
```

Die Auswahl des Rads ist **öffentlich lesbar und setzbar**. Damit braucht es
nirgends eine Eingabe-Unterdrückung — und genau daran scheitern solche
Vorhaben sonst. Die Recherche zur Originalmod hält fest, dass das Rad im
Gameplay-Kontext läuft, wo `LShoulder`/`RShoulder` gar nicht existieren;
gelesen wird deshalb direkt über XInput (Beobachten ist harmlos).

**Warum nicht das Steuerkreuz**, obwohl es naheliegt: Die zwölf Slots sind auf
dem Rad vier Richtungen mal drei Ringe, nachzulesen in den Konstanten von
`FavoritesMenu.as` — `FS_LEFT_3=0`, `FS_LEFT_2=1`, `FS_LEFT_1=2`,
`FS_RIGHT_1=3` … `FS_DOWN_3=11`, mit eigenen Fokusgrafiken
`UIMenuQuickUseFocusDpadA/B/C` pro Ring. Im Raster liegen Slot 3 und 4
nebeneinander, auf dem Kreuz sind es gegenüberliegende Seiten. Ein linearer
Schritt nach rechts ist dort kein Richtungswechsel. Dazu bewegt das Spiel
`selectedIndex` bei jedem Kreuz-Druck selbst — wir müssten die Vanilla-Reaktion
unterdrücken, und genau daran scheitern solche Vorhaben (siehe die Notizen zum
`BSInputEnableManager` aus dem VATS-Projekt: zu grob, schaltet Nachbarfunktionen
mit ab).

**Vorgesehene Belegung:**

- **Schultertasten** → `selectedIndex` linear ±1 durch die Zeile. Passt besser
  zum Raster als das Vanilla-Kreuz, das in Himmelsrichtungen springt.
- **Trigger LT/RT** → Zeile hoch/runter. `leftTrigger`/`rightTrigger` lagen in
  der XInput-Struktur schon vor, ungenutzt.
- **A** → benutzen, unverändert vanilla.
- **Steuerkreuz** macht weiter sein Vanilla-Ding und schreibt dieselbe
  Variable; stört also nicht.

**Was das Grid dafür braucht:** Es muss `selectedIndex` *anzeigen* — bisher
kennt es nur den Mauszeiger. Eine hervorgehobene Zelle, sonst sieht der
Controller-Nutzer nicht, wo er ist.

**Am Geraet gemessen (2026-09-02, Xbox-Controller).** Das Grid ist mit
Controller schon benutzbar: Das Menue oeffnet, und das Spiel behandelt es wie
das normale Rad. Das Steuerkreuz wandert **links/rechts durch die Slots 1-6
und hoch/runter durch 7-12** — genau die Aufteilung, die die `FS_*`-Konstanten
vorgeben (`FS_LEFT_3`..`FS_RIGHT_3` = 0..5, `FS_UP_*`/`FS_DOWN_*` = 6..11).
Damit ist gemessen, was oben nur aus dem Quelltext geschlossen war, und der
Einwand gegen das Steuerkreuz ist bestaetigt: Was im Raster nebeneinander
liegt, liegt auf dem Kreuz auf gegenueberliegenden Seiten. Die weiteren Reihen
ignoriert es erwartungsgemaess (das Rad kennt nur zwoelf Slots), und eine
Markierung der Auswahl fehlt — beides genau die zwei Luecken, die der Entwurf
oben fuellen will.

### Schritt 1 gebaut: das Grid zeigt die Auswahl (2026-09-02)

Vor jeder Eingabe kommt die Anzeige — sie beweist die tragende Annahme und
ist sofort sichtbar, ganz ohne XInput.

`ReadSelectedIndex` liest `selectedIndex` schlicht als Eigenschaft von
`menu->menuObj`; `ReadNumber` deckt die `UInt`-Form ab, in der sie kommt.
`kNoSelection` ist 12, das `FS_NONE` der ActionScript-Seite. `MoveSelection`
zeichnet einen **eigenen** Marker namens `GridSelection` — bewusst als Umriss
statt als Fläche, damit bei Maus **und** Controller auf dem Schirm
unterscheidbar bleibt, welcher der beiden was treibt. Der vorhandene
Highlight kam dafuer nicht in Frage: Er haengt an Cursor-Ereignissen, die ein
Controller nie sendet.

Gezeichnet wird nach den Zellen (liegt also oben) und auf der **aktiven
Reihe**, weil das die Seite ist, die das Rad zeigt. Liegt der Index nicht in
`geometry.pageSlots`, wird der Marker versteckt statt geraten.

48 Tests gruen, gebaut und deployed. **Ungetestet.** Der Test braucht nur den
Controller: Steuerkreuz druecken, der Umriss muss mitwandern — links/rechts
durch die Slots 1-6, hoch/runter durch 7-12.

### Schritt 2 gebaut: das Steuerkreuz gehoert jetzt dem Raster (2026-09-02)

**Der Entwurf mit Schultertasten und XInput ist verworfen.** Alexanders
Einwand traf: Die Vanilla-Reaktion muss gar nicht unterdrueckt werden, weil
**die Mod die SWF selbst ausliefert**. Was die Tasten bedeuten, wird also
schlicht im ActionScript umgeschrieben. Kein XInput, kein Polling, keine
Eingabeunterdrueckung — und die Pfeiltasten der Tastatur verhalten sich als
Zugabe gleich.

**Belegung:** hoch/runter wechselt die **Reihe**, links/rechts den **Slot**
(linear, an beiden Enden geklemmt statt umlaufend — auf zwoelf Zellen liest
sich Umlaufen wie ein Fehler). Beim Oeffnen landet die Auswahl immer auf
Slot 1, statt auf dem Platz, den die gedrueckte Richtung im Rad getroffen
haette.

Verworfen wurde dabei Alexanders eigener zweiter Vorschlag (hoch/links/rechts/
runter waehlen beim Oeffnen Reihe 1/2/3/4): Er setzt genau vier Reihen voraus,
`RowCount` geht aber von 2 bis 8. Bei zwei Reihen zeigten zwei Richtungen ins
Leere, bei acht waere die Haelfte per Geste unerreichbar.

**Teile:**
- `SwitchRowHandler` in `favorites_ui.cpp`, registriert als
  `FavoritesBanksSwitchRow` — dasselbe Muster wie `FavoritesBanksClearSlot`.
  Nimmt ein Vorzeichen, klemmt auf `rowCount`, ruft `QueueBankSwitch`.
- `FavoritesBanksStepSlot` in `FavoritesMenu.as`. Rechnet als `int`, weil
  `selectedIndex` ein `uint` ist: Null zu dekrementieren liefe sonst auf vier
  Milliarden ueber.
- Beide Zweige haengen an `this.FavoritesBanksSwitchRow != null`. Fehlt der
  Rueckruf (neue SWF, alte DLL), greifen die alten Richtungstabellen
  unveraendert.

**`swf-src/scripts/` ist ab jetzt Quellcode, keine Referenz mehr.**
`build_swf.py` baut die SWF neu: exportiert alle 59 Skripte mit FFDec, tauscht
nur die ein, die wir pflegen, importiert zurueck und legt eine `.bak` an.
Vorher wurde geprueft, dass ein Durchlauf **ohne** Aenderung zeichengleich
zurueckdekompiliert — das Werkzeug selbst veraendert also nichts.

**Erster Test, zwei Nachbesserungen (2026-09-02).** Links/rechts durch die
Slots und hoch/runter durch die Reihen funktionierte sofort. Zwei Dinge nicht:

1. **Die Auswahl startete nicht auf Slot 1.** Die Richtung, mit der das Rad
   geoeffnet wird, kommt **zusaetzlich als Tastenereignis** an und wurde
   sofort als Bewegung ausgefuehrt. `FavoritesBanksFreshOpen` verschluckt
   jetzt den ersten Richtungsdruck nach dem Oeffnen.
2. **Der Reihenwechsel warf die Auswahl auf den Anfang zurueck.** Die SWF
   setzt beim Wechsel der gezeichneten Reihe `selectedIndex = FS_NONE` — im
   Rad sinnvoll, dessen Ringe je Seite andere zwoelf sind; im Raster sind es
   immer dieselben zwoelf. Der Slot wird jetzt um die Aktualisierung herum
   gesichert und wiederhergestellt (nicht einfach stehengelassen, damit die
   Entry-Clips ihre Auswahlmarkierung mitbekommen).

**Neue Option `[Controls] WrapNavigation`** (Standard 0, Alexanders
Vorschlag): Enden als Wand oder als Tuer — Slot 12 nach rechts bleibt stehen
oder landet auf Slot 1, Reihe 1 nach oben ebenso. Gilt fuer Slots **und**
Reihen. Diese Option ist von anderer Art als die vier ausgebauten: Es gibt
keine richtige Antwort, nur einen Geschmack. Genau dafuer sind Optionen da.

**Offen, noch nicht angefasst:** Mit `ExternallyManagedSlots` wandert der
Schritt weiterhin durch alle zwoelf Indizes, auch durch reservierte — die
aber nicht in der Reihe gezeichnet werden, sondern im Streifen rechts. Der
Marker versteckt sich dann (er zeigt nur, was in `geometry.pageSlots` steht).
Bei Alexander faellt das nicht auf, er hat `ExternallyManagedSlots=0`.

**Ungetestet.** Zu pruefen: wechselt hoch/runter die Reihe, wandert
links/rechts durch die Slots, und startet die Auswahl auf Slot 1.

**Weiter ungeprueft:** ob das *Setzen* von `selectedIndex` die sichtbare Markierung
mitzieht und ob A danach den richtigen Slot benutzt. Der XInput-Code wurde
beim Aufräumen entfernt und müsste aus der Git-losen Historie oder aus
`FavoritesBanks-0.7.5-source/src/main.cpp` zurückgeholt werden.

## 1. Wo liegt was

| Was | Pfad |
|---|---|
| Arbeitskopie (Quellcode) | `Claude Code/FavoritesBanks-0.7.6-source/` |
| Abhängigkeit (Sibling, Pflicht) | `Claude Code/CommonLibSF-libxse/` |
| Referenz: pristine 0.7.5 | `Claude Code/FavoritesBanks-0.7.5-source/` |
| Referenz: offizielles 0.7.6-Paket | `Claude Code/FavoritesBanks-0.7.6-mainfile/` |
| Paketordner (DLL+SWF+INI) | `Claude Code/FavoriteBanksCompiled/` |
| Spiel | `G:\...\Starfield\Data\` |
| Log | `Documents\My Games\Starfield\SFSE\Logs\FavoritesMenuGrid.log` |
| Sidecar-State | `Documents\My Games\Starfield\SFSE\Plugins\FavoritesBanks\States\character-*/current.fbs` |

## 2. Bauen und Deployen

```bash
xmake                    # Build (xmake liegt in C:\Program Files\xmake)
py -3 -m unittest discover -s tests
py -3 deploy.py          # kopiert DLL + SWF ins Spiel, INI wird gemerged
```

`deploy.py` überschreibt **niemals** bestehende INI-Werte; neue Optionen
werden in ihre richtige Sektion einsortiert. **Starfield muss geschlossen
sein**, sonst bricht es sauber ab. Wenn das Spiel läuft, hilft ein
Hintergrund-Wächter:

```bash
until ! tasklist //FI "IMAGENAME eq Starfield.exe" | grep -qi starfield.exe; do sleep 10; done
py -3 deploy.py
```

Die Toolchain wurde von Null aufgesetzt: VS Build Tools 2022 (MSVC 14.44),
xmake, CommonLibSF auf Commit `b5817cf` **inklusive Submodul**
`lib/commonlib-shared` (`git submodule update --init --recursive` — fehlt es,
bricht xmake mit `unknown target(commonlib-shared)` ab).

## 3. Eigene Features (nicht in der offiziellen Mod!)

Bei jedem Upstream-Update **neu einmergen**. Zeilenenden vorher angleichen,
sonst sind die Diffs unbrauchbar (Upstream liefert CRLF).

| Feature | INI | Kern |
|---|---|---|
| Prev/Next-Page-Tasten | `PrevPageKey`, `NextPageKey` | `main.cpp` KeyboardPollingLoop |
| Reset auf Seite 1 beim Schließen | `ResetToFirstPageOnClose` | `QueueCommitActiveBank` |
| Equip-Toggle | `ToggleEquipOnSelect` | `TryToggleUnequipLocked` |
| Extern verwaltete Slots | `ExternallyManagedSlots` | Reconcile + Commit + `g_pinnedSlots` |
| Grid-UI (komplett) | `[Grid]` | `favorites_grid.cpp` (neue Datei) |

Tests in `tests/test_model.py` sichern diese Features ab (38 Tests, alle grün).

## 4. Gelöste Probleme — mit Ursache

**Favoriten wanderten selbstständig ans Ende des Rads.**
`ReconcileNativePageWithBank` löschte jeden gespeicherten Deskriptor, dessen
nativer Slot leer gelesen wurde. Skriptverwaltete Items (Gadget-Mods,
Follower-Token) verschwinden kurz aus dem Inventar und kommen zurück; die
Engine vergibt dann einen freien Slot. Fix: `DescriptorItemIsCarried` +
Erkennung von Umhängungen. **Wichtigster Auslöser war aber ein anderer
Slot-6-Konflikt** → `ExternallyManagedSlots=6`.

**Ein Item stand auf allen vier Seiten in Slot 6.**
Eine Gadget-Mod schrieb ihr Item dorthin; der Reconcile übernahm es in jede
besuchte Seite. Erkennbar daran, dass die Einträge **kein VISUAL** hatten —
das entsteht nur bei echter Zuweisung über das Rad.

**Crash beim ersten Grid-Build.** 45 Displayobjekte aus dem
SFSE-Menü-Callback erzeugt — der läuft **nicht auf dem Main-Thread**, und
Scaleform ist nicht thread-safe. Immer über `SFSE::GetTaskInterface()->AddTask`.
Ein Test (`test_grid_is_never_built_off_the_main_thread`) hält das fest.

**Text war unlesbar ("Punkte").** Ohne gesetzten Font nimmt Scaleform einen
Fallback, der als Punktraster rendert. Der richtige Name steht in der
entpackten `favoritesmenu.swf` (zlib, `CWS`-Header): **`$MAIN_Font_Bold`**,
Bibliothek `fonts_en`. Die Font-Probe misst alle Kandidaten und wählt.

**Kreise und Kreuzschatten blieben sichtbar.** Es sind unbenannte
Timeline-Grafiken; Flash nennt sie `instance1`…`instance4`. Sie haben keine
ansprechbaren Namen — die Kinderliste durchgehen und alles ausblenden, was
mit `instance` beginnt.

**Zwei Drittel der INI wurden nicht mehr gelesen.** `LoadSettings` in
`main.cpp` las nur noch `BankCount`, `ShowIndicator`, `IconTelemetry` und die
drei `[Grid]`-Schalter. Beim Entfernen der Hotkey-Buttons ist der umgebende
Block mitgegangen; `ParseExternallyManagedSlots` stand noch da, wurde aber
nirgends aufgerufen. Sichtbar wurde es als **verlorener Slot-6-Button**: ohne
`ExternallyManagedSlots` gibt es keinen gepinnten Slot und damit keine Zelle
über dem EDIT-Umschalter. Genauso still betroffen: `MouseWheel`, `Controller`,
`IconSize`, `PollIntervalMs`, alle Tastenbelegungen. Kein Fehler, keine
Warnung — die Werte waren einfach die Defaults. **Ein Test hält das jetzt
fest** (`test_every_documented_ini_key_is_actually_read`): jede Zeile mit `=`
in der ausgelieferten INI muss in `main.cpp` einen Leser haben.

**Rechtsklick im Edit-Modus stürzte ab.** Zwei Ursachen übereinander.
Erstens erreicht *ein* Rechtsklick **beide** Listener — deshalb stand im
Crashlog `InventoryMenu`, obwohl nur rechts geklickt wurde: der Linkspfad war
mitgelaufen und hatte das Inventar mit angefordert. Zweitens rief
`OpenGameMenu` `ProcessUserEvent("Cancel")` **aus dem ActionScript-Dispatch
heraus** auf, der gerade lief: das reißt das Movie ab, dessen Handler noch auf
dem Stack steht. Fix: Der Handler prüft `event.type` und lehnt fremde Events
ab, das Öffnen läuft über `AddTask`, und ein einmaliger Merker lässt pro
Öffnen des Rads nur eine Menü-Anforderung durch.

**Scaleform liefert hier kein `rightClick`.** Beide Maustasten kommen als
gewöhnlicher `click` an. Deshalb landete ein Rechtsklick zuerst im
Links-Handler (und öffnete das Inventar), und tat gar nichts mehr, sobald
dieser Handler fremde Event-Typen ablehnte — der zweite Listener feuerte nie.
Die Taste wird jetzt bei `mouseDown` per `GetAsyncKeyState(VK_RBUTTON)`
abgetastet, solange sie noch gedrückt ist; `click` wird erst beim Loslassen
zugestellt, da ist nichts mehr unten. Der Handler protokolliert den Event-Typ
einmal pro Sitzung (`Grid click: event type ...`), falls sich das je ändert.

**Menü öffnen und Rad schließen dürfen nicht in denselben Frame.** Der erste
Versuch schob nur das Schließen über `AddTask` hinaus und öffnete das Menü
sofort danach — es krachte weiter, an derselben Adresse, mit der Menüliste
und einer halb registrierten `InventoryMenu_StartCloseAnim`-Eventquelle auf
dem Stack. Das Rad wird jetzt zuerst gecancelt, und der `kShow` geht erst
raus, wenn `UI::GetMenu("FavoritesMenu")` null liefert (Frame für Frame, mit
Obergrenze). Im Log steht dann `Grid opened X after N frame(s)`.

**Das Grid baute sich auf einem sterbenden Menue neu auf.** Klick auf eine
leere Zelle cancelt das Rad; die Polling-Schleife rief `UpdateGridOverlay`
aber weiter auf, solange Icons ausstanden, fand kein Overlay mehr und legte
per `BuildOverlay` ein neues auf einer Stage an, die die Engine gerade
abraeumte -- Absturz mit `BuildOverlay` im Backtrace. `UpdateGridOverlay`
steigt jetzt sofort aus, solange `g_favoritesMenuVisible` false ist, und baut
nie, waehrend ein Menuewechsel aussteht.

**`GetMenu` ist kein Test auf "offen".** Ein gecanceltes FavoritesMenu bleibt
abrufbar; das Warten lief 90 Frames leer und das Powers-Menue ging nie auf
(`Grid gave up opening PowersMenu`). Gewartet wird jetzt auf
`g_favoritesMenuVisible`.

**Das X loeschte nur auf der aktiven Seite.** `ClearSlotFromMenu` beginnt mit
`if (bank != g_activeBank) return;`. Fuer das Rad ist das richtig -- es kann
gar nichts anderes fragen -- fuer das Grid ist es toedlich: die Loeschecke tat
auf drei von vier Zeilen nichts, und zwar schweigend. Es gibt jetzt
`ClearGridSlot` ohne diese Schranke (`ClearSlotAt(index, onlyActiveBank)`);
die ENTF-Taste benutzt sie ebenfalls.

**Nach Tausch oder Loeschen blieb das alte Icon stehen.** Eine ImageFixture
laedt einmal und behaelt, was sie geladen hat. Jede Zelle merkt sich jetzt,
was sie zeigt (`IconFit::key` = FormID plus Bildname); aendert sich das, wird
der Clip entfernt und neu gebaut. Das war der Grund, warum ein erfolgreicher
Tausch aussah, als sei nichts passiert.

**`ProcessUserEvent("Cancel")` wirkt nur aus dem AS-Dispatch heraus.** Aus
einem Task aufgerufen tut es gar nichts -- das Rad blieb offen, das Warten
lief leer, und im Log stand `the favorites menu never closed`. Geschlossen
wird jetzt ueber `UIMessageQueue` mit `kHide`.

**`SFSE::GetTaskInterface()->AddTask` verzögert hier nichts.** Der Beweis
steht im Log, auf die Millisekunde:

```
15:49:41.032  Grid asked the wheel to close, to open PowersMenu
15:49:41.032  Grid gave up opening PowersMenu: the favorites menu never closed
```

Neunzig „warte einen Frame"-Versuche, die sich gegenseitig per `AddTask`
einreihen, liefen innerhalb derselben Millisekunde durch. Die Aufgabe wird
also **sofort auf dem aufrufenden Thread** ausgeführt. Damit ist „pack es in
einen Task" als Verzögerungsstrategie hinfällig — und es erklärt rückwirkend,
warum unsere Zeilen trotz `AddTask` immer von einem anderen Thread kamen.

Wer hier wirklich auf Frames warten kann, ist die eigene Polling-Schleife in
`main.cpp` (10 ms). Der Menüwunsch wird jetzt geparkt (`g_pendingMenu`, mit
5-Sekunden-Verfall), das Rad per `kHide` geschlossen, und die Schleife
schickt beim Übergang „Rad war offen → Rad ist zu" das `kShow`.

**Klick auf leere Zelle ist raus, dafür zwei Buttons.** `ITEMS` und `POWERS`
unter dem EDIT-Umschalter. Der alte Weg brauchte einen Rechtsklick, den
Scaleform hier gar nicht meldet, und war unauffindbar. Die ganze
Maustastenerkennung (`GridButtonProbe`, `g_rightButtonDown`, der
`mouseDown`-Listener) ist damit entfallen. Eine leere Zelle ist jetzt nur
noch Ablagefläche.

**Der Streifen war unterhalb der letzten Seitenzeile nicht klickbar.**
`CellAt` und der Klick-Handler verwarfen jede Zeile `>= geometry.rows` — auch
im Streifen, der länger sein darf als die Seiten. Bei vier Seiten und vier
Streifeneinträgen ging es zufällig auf; mit zwei gepinnten Slots oder zwei
Seiten wären die unteren Buttons tot gewesen.

**Menüs lassen sich von hier aus nicht öffnen — endgültig.** `kShow` auf
`InventoryMenu` über die `UIMessageQueue` reißt das Spiel mit, und zwar
*auch dann*, wenn das Rad vorher sauber geschlossen und die Seite committet
wurde. Log:

```
16:34:12.029  Grid asked the wheel to close, to open InventoryMenu
16:34:12.059  Committed page 3 into the native favorite slots
16:34:12.077  Grid opened InventoryMenu now that the wheel has closed
              -> Crash, Main-Thread, Starfield.exe+0x15CB54C
```

Der Ablauf war also am Ende korrekt: Rad zu, Seite gesichert, Menü angefordert
— und die Engine stirbt trotzdem an derselben Adresse wie beim allerersten
Versuch. `InventoryMenu` braucht offenbar Kontext, den die reine Nachricht
nicht mitbringt (es ist Teil der Pausenmenü-Maschinerie, nicht ein
freistehendes Menü wie `FavoritesMenu`).

**Konsequenz:** Das Feature ist raus, mitsamt `OpenGameMenu`, der
Warteschlange dafür, den ITEMS/POWERS-Buttons und der ganzen
Maustastenerkennung. Beide Menüs sind im Spiel ohnehin einen Tastendruck
entfernt. **Nicht erneut versuchen**, ohne vorher herauszufinden, wie das
Spiel dieses Menü selbst öffnet — die Nachricht allein reicht nicht.

**Streifen-Seite: gebaut, gescheitert, entfernt.** Eine Streifenzelle sollte
einer Seite gehoeren, die keine Zeile hat, um einen festen Platz zu bekommen
ohne einen der zwoelf Slots zu reservieren. Die Zellen blieben leer und liessen
sich nicht befuellen; der Verdacht, sie haette Seite 4 kopiert, hat sich als
falsch erwiesen (das war der Abgleich beim Laden). Alexander hat sie ad acta
gelegt, der Code ist raus -- inklusive `TotalBankCount()`, `StripBank()`,
`g_lastVisibleBank` und der Ausnahme beim Schliessen des Rads.

**Falls jemand es wieder versucht:** Der Ansatz selbst ist nicht widerlegt, nur
nie zum Laufen gekommen. Was fehlte, war nachweisbar das Befuellen -- im Log
stand keine einzige `Moved`-Zeile fuer die Streifen-Seite.

**keine Zeile hat**: `StripBank() == bankCount`, also bei `BankCount=4` intern
Seite 5, und sie wandert mit, wenn Seiten dazukommen. Ein Klick darauf wechselt
auf diese Seite, genau wie ein Klick in eine Zeile auf deren Seite wechselt.

Warum das nichts kostet: Solange das Favoritenmenü offen ist, drückt niemand
eine Zifferntaste. Beim Schließen setzt `QueueCommitActiveBank` die Seite
zurück, auf der der Spieler wirklich war (`g_lastVisibleBank`) — die
Streifen-Seite darf **nie** in den echten Slots stehen bleiben, sonst ist das
Rad fast leer. Ein Test hält das fest
(`test_the_strip_page_never_stays_in_the_real_slots`).

Wichtig beim Anfassen:

- **Tastatur und Mausrad zählen `bankCount`, nicht `TotalBankCount()`.** Nur
  so ist die Streifen-Seite unerreichbar. Der Test verbietet `TotalBankCount`
  in `main.cpp` ausdrücklich.
- **Speichergrenzen zählen `TotalBankCount()`**: `SwitchBank`,
  `QueueBankSwitch`, `MaterializeBankLocked`, `ClearSlotAt`,
  `SwapStoredSlots`, `SelectGridCell` und die Klammer beim Laden.
- `kMaxBanks` ist 8, also `BankCount<=7` mit Streifen-Seite; darüber wird
  gekappt (mit Warnung).
- **Befüllt** wird eine Streifenzelle nur im Edit-Modus: aufheben, auf die
  Zelle klicken. Ihre Seite hat keine Zeile, es gibt also keinen anderen Weg
  dorthin. Rotes X funktioniert dort ebenfalls.

**Seite 4 wurde nach jedem Neustart überschrieben — und die Streifen-Seite war
unschuldig.** Die zwei Zeilen aus dem Log sagen alles:

```
state loaded: 4 stored bank(s), page 4 shown, page 4 recorded in the native slots
[W] Loaded native page did not exactly match any stored bank; reconciling it with bank 4
```

Der Spielstand hieß `Save440_..._20260830012438` — 01:24 Uhr, zwanzig Stunden
vor den Änderungen in der Sidecar-Datei. Alexander spielt ohne zu speichern
(er beendet Starfield über den Task-Manager). Beim Laden treffen also die
zwölf echten Slots von 01:24 auf eine Seitenverwaltung von 21:40, und die Mod
schreibt die alten Slots in die Seite, die sie als nativ notiert hat.

**Das ist kein Fehler, sondern die Konsequenz aus einer SFSE-Lücke.** SKSE und
F4SE haben eine Serialisierungs-Schnittstelle für Co-Saves; SFSE nicht — seine
`LoadInterface` kennt nur `kMessaging`, `kTrampoline`, `kMenu`, `kTask`. Ein
Plugin kann nichts *in* den Spielstand schreiben, also muss der Zustand
daneben liegen, und damit können beide aus verschiedenen Zeiten stammen.
Favorites Extended hat dasselbe Problem, aus demselben Grund.

Gegenmittel für den Nutzer: nach dem Sortieren einmal speichern. Als
Codeänderung wurde `TrustSidecarOnLoad` erwogen (bei Nichtübereinstimmung die
notierte Seite in die echten Slots zurückschreiben statt umgekehrt) und von
Alexander **abgelehnt** — er will, dass der Spielstand die Wahrheit bleibt.
Nicht ungefragt nachbauen.

## 5. Offene Probleme

### 5a. Icon-Darstellung

Der **Crash** ist weg: die eigenen Puffer pro Zeile
(`FavoritesBanksGrid0`…) statt des gemeinsamen `FavoritesIconBuffer` waren
die Lösung. `FavoritesEntry` lädt hart in diesen einen Puffer, der für die
zwölf Slots des Rads ausgelegt ist; das Grid will 45, und es krachte
reproduzierbar nach 16–20 Icons. Jetzt wird `Components.ImageFixture` direkt
gefahren, mit selbst gewähltem Puffernamen.

Geblieben war die **Darstellung**: Icons flogen beim Öffnen quer über den
Bildschirm, saßen daneben, Power-Symbole waren zu klein. Drei Ursachen,
alle am 2026-08-30 behandelt, **noch nicht im Spiel geprüft**:

1. *Fliegen.* Eine frisch erzeugte Fixture wird in ihrer eigenen Größe
   gezeichnet — bei manchen Waffen mehrere hundert Pixel — bis die erste
   erfolgreiche Messung greift. Sie startet jetzt `visible=false` und wird
   erst sichtbar, wenn sie gemessen ist.
2. *Position.* Gemessen wird jetzt mit `getBounds(self)` statt mit
   `width`/`height`. Die beiden beschreiben die Box **relativ zum
   Registrierungspunkt**, und eine zentrierte Fixture hängt ihr Bild um
   diesen Punkt herum — die linke obere Ecke liegt also im Negativen.
   Positionieren über `x`/`y` allein landet dann eine halbe Bildgröße
   daneben. `getBounds` sagt, wo das Bild wirklich liegt.
3. *Power-Icons zu klein.* `clipSizer = "Sizer_mc"` ist eine Hilfsbox, die
   das Rad der Fixture gibt; sie ist **größer als das Symbol darin**. Wer
   die Fixture misst, misst die Box — das Symbol wurde auf diese skaliert
   und blieb sichtbar kleiner als jedes Item-Icon daneben. Das Grid setzt
   `clipSizer` und `centerClip` jetzt für alles auf leer/false und fittet
   selbst.

Punkt 1 und 2 haben gewirkt (2026-08-30 bestaetigt: kein Fly-in mehr,
Position stimmt). **Punkt 3 war falsch diagnostiziert.** Die Telemetrie hat
es dann verraten -- fuer *jedes* Power-Icon dieselbe Zahl:

```
Grid icon 'Void Form' (power): bounds 1275x600 at (-625,-300), scaled 0.04 into a 66 cell
```

Ein konstanter 1250x600-Kasten um den Registrierungspunkt, unabhaengig vom
Symbol. Gemessen wurde also nicht das Bild, sondern ein Layout-Element in der
Fixture; auf die Zelle gefittet wurde folglich der Kasten, und das Symbol kam
mit vier Prozent Skalierung heraus. `MeasureArtwork` misst jetzt die
**Kinder** der Fixture und laesst alles weg, dessen Name nach Layout klingt
(`sizer`, `bound`, `background`, `bg`, `hit`, `frame`, `mask`); der Rest ist
das Bild. Der Kindbaum des ersten Icons steht einmal pro Sitzung im Log
(`Grid icon child[n] ...`) -- falls der Kasten anders heisst, steht dort sein
Name.

**Weitere Rückfallposition:** Die Klassenprobe testet auch
`flash.display.BitmapData` und `flash.display.Bitmap` (Ergebnis im Log unter
`Grid probe:`). Damit ließe sich zeilenweise laden, in eine Bitmap kopieren
und die Fixture freigeben.

### 5b. Zeichnen auf dem Movie-Thread (gebaut, ungetestet)

Crash vom 15:35:

```
Thread: IOManager
Exception Address: 0x756E654D73      <- keine Adresse, sondern Text: "sMenu"
R10/R12: Scaleform::GFx::AS3::ASVM*
```

Die AS3-VM ist in eine Zeichenkette gesprungen — Speicherkorruption
*innerhalb* der UI-VM. Der Hinweis auf das Warum stand im eigenen Log: jede
Zeile trägt die Thread-ID, und die Zeilen des Grids kamen jedes Mal von einer
anderen (15340, 17856, 21600, 23456 …), während der Start auf 1788 lief. Wir
gingen überall über `AddTask` — und landeten trotzdem nicht auf einem festen
Thread. Scaleform ist nicht thread-sicher; die Crashes auf
`CreationRenderer`, `BSJobs` und `IOManager` passen alle dazu.

**Umbau:** `UpdateGridOverlay` zeichnet nicht mehr. Es setzt nur
`g_redrawRequested`. Gezeichnet wird in `DrawGridOverlay`, und das ruft
ausschließlich `GridTicker` auf — ein `enterFrame`-Listener auf unserem
Overlay. Den ruft die Engine aus ihrem eigenen Advance dieses Movies auf,
also per Konstruktion zum richtigen Zeitpunkt auf dem richtigen Thread.

Details, die dabei wichtig waren:

- **Bootstrap.** Das Overlay, das den Listener trägt, muss einmal gebaut
  werden. Solange noch kein Tick angekommen ist (`g_tickerAlive` false),
  zeichnet `UpdateGridOverlay` wie bisher direkt. Das ist zugleich das
  Sicherheitsnetz, falls `enterFrame` in dieser Scaleform-Fassung gar nicht
  feuert — ein Grid, das nicht ticken kann, muss trotzdem erscheinen.
- **Die Anforderung darf nicht verloren gehen.** Der Ticker löscht das Flag
  *nicht*; das tut der Zeichenvorgang, sobald er die `drawing`-Sperre hält.
  Sonst frisst ein Tick, der auf eine laufende Zeichnung trifft, die
  Anforderung.
- **`g_iconFits` gehört dem Zeichenvorgang.** Es wird nur in `BuildOverlay`
  geleert, innerhalb der Sperre — nicht in `ResetGridSession`, die vom
  Polling-Thread kommt.
- **`ResetGridSession`** wird beim Schließen des Rads aus der Polling-Schleife
  gerufen und setzt Ticker, Edit-Modus und Menü-Merker zurück.

Ein Test (`test_the_grid_draws_only_from_the_movies_own_frame_tick`) hält
fest, dass `UpdateGridOverlay` selbst nichts mehr zeichnet.

**Ergebnis (2026-08-30, 15:48):** Der Ticker läuft — die Zeile
`Grid: the movie's frame ticker is running` steht bei jedem Öffnen im Log.
Die Thread-IDs sind aber **weiterhin verschieden**. Scaleform advanced dieses
Movie in Starfield also selbst auf einem Worker-Pool; „ein fester Thread" war
die falsche Erwartung. Der Gewinn bleibt trotzdem: Wir arbeiten jetzt
innerhalb des Advance, den die Engine für dieses Movie ausführt, also nicht
mehr *neben* ihrem eigenen Zugriff darauf. Ob das die Crashes beseitigt, ist
noch offen — es braucht eine längere Sitzung.

Frühere Notiz dazu: **Fehlt diese Zeile, ist der Umbau wirkungslos** — dann feuert `enterFrame`
nicht und es braucht einen anderen Aufhänger (z. B. ein AS-seitiges
`addEventListener` in der eigenen SWF).

### 5c. Grid-Buttons lösen nichts aus

Technisch korrekt: Log zeigt `pressed Ctrl+0x82 ... (4 of 4 accepted)`.
Aber **Starfield Hotkeys (reg2k, Nexus 1578) ist kein Windows-Listener** —
`enable_hotkeys.cmd` schaltet eine in der Engine eingebaute Funktion frei,
konfiguriert über den `hotkey`-Konsolenbefehl. Die Tastenerkennung liegt
unterhalb dessen, was `SendInput` erzeugt. **Sackgasse.**

Alternative wäre `IVirtualMachine::DispatchStaticCall` (entspricht `cgf`).
Bewusst nicht gebaut: CommonLibSF hat keinen Wrapper, `BSTThreadScrapFunction`
und `IStackCallbackFunctor` müssten von Hand gebaut werden — zu crashanfällig
für den Nutzen. Der Nutzer hat die Hotkeys ohnehin auf der Maus.

Der Platz soll stattdessen den **Edit-Modus** tragen (siehe unten).

### 5d. Info-Panel saß unter dem Grid

Ursache gefunden: `ItemInfo_mc.y` ist **Menü-Koordinate**, nicht
Bühnenkoordinate (`y=-330` bei Position oben im Bild, weil der Menü-Ursprung
die Mitte ist). Ich hatte beide Systeme vermischt. Fix rechnet jetzt über
`menuObj.y` um — **noch ungetestet**.

## 6. Grid-UI: aktueller Funktionsumfang

Layout: eine Zeile pro Seite, eine Zelle pro Slot, rechts ein Streifen mit
gepinnten Slots und dem Edit-Umschalter. Native Zeile ist hervorgehoben
(dort kostet ein Klick keinen Commit).

- **Klick** — wechselt zur Seite und benutzt den Slot, über
  `FavoritesMenu.ProcessUserEvent("Quickkey<N>")`. Das ist public und der
  Eingang, den die Quickkey-Tasten nehmen; dadurch erbt das Grid alle
  bestehenden Sicherungen.
- **Hover** — Highlight, eigene Statuszeile plus das vanilla `ItemInfo_mc`.
- **ENTF** — löscht die Zelle unter dem Cursor (die Rad-Taste wirkt auf die
  Rad-Markierung, die das Grid nicht setzt).
- **Edit-Modus** — funktioniert, inklusive seitenübergreifend (Log:
  `Moved 'Void Form' (page 2 slot 12) and 'Solar Flare' (page 2 slot 11)`).
  Nur sah man es nicht: aufgenommen wurde bislang allein durch die grüne
  Quellzelle angezeigt. Die Statuszeile sagt jetzt
  `HOLDING <Name> - click any cell to put it there.` Klick hebt auf, zweiter Klick tauscht —
  auch seitenübergreifend. Rotes X oben rechts löscht. Leere Zelle:
  Linksklick → Inventar, Rechtsklick → Powers-Menü. Ist die native Seite
  beteiligt, werden die nativen Slots sofort nachgezogen
  (`MaterializeBankLocked(bank, force=true)`), sonst überschreibt die nächste
  Erfassung die Änderung; schlägt es fehl, wird zurückgetauscht.

## 7. Fallstricke

- **Scaleform nur vom Main-Thread.** Immer `AddTask`.
- **Nichts aus einem AS-Handler heraus schließen.** Ein Menü zu canceln,
  während sein eigener Event-Dispatch auf dem Stack liegt, ist ein
  Use-after-free. Über `AddTask` hinausschieben.
- **Die Fixture ist groesser als ihr Bild.** Nie die Fixture messen,
  sondern ihre Kinder ohne die Layout-Helfer.
- **Es gibt kein `rightClick`-Event.** Beide Maustasten kommen als
  `click`; die Taste bei `mouseDown` selbst abtasten.
- **Menüwechsel brauchen zwei Phasen.** Erst schließen, dann warten, bis
  `GetMenu` null liefert, dann das neue öffnen. Beides im selben Frame ist
  ein Zugriff auf die Menüliste, während die Engine darin aufräumt.
- **Neue INI-Optionen brauchen einen Leser in `LoadSettings`** — der Test
  besteht darauf, aber nur, wenn der Schlüssel auch in der INI steht.
- **`numChildren` ist `int`, nicht `Number`.** `IsNumber()` schlägt fehl —
  über `ReadNumber` lesen, sonst schweigt die Diagnose.
- **Vorwärtsdeklarationen** in `favorites_grid.cpp` nötig; `[[nodiscard]]`
  darf nur an der Deklaration stehen.
- **`REX::DEBUG` erscheint nicht im Log.** Für Diagnose `REX::INFO`.
- **Die SWF ist zlib-komprimiert** (`CWS`) — `strings` findet nichts, erst
  ab Byte 8 dekomprimieren.
- **Sektionsnamen der INI sind fix**; Umbenennen macht Nutzerwerte
  unauffindbar und setzt still auf Standard zurück.
- `MaterializeBankLocked` ist der Pfad, der in 0.6.0 echte Favoriten
  gelöscht hat. Änderungen dort mit Bedacht.

## 8. Nutzerkontext

- Spielt mit vielen Skript-Mods, die Items ins Inventar geben und nehmen —
  das ist die Quelle mehrerer Bugs.
- Maustasten senden F-Tasten; `Ctrl-F19`/`Ctrl-F20` sind belegt.
- Will **alle** Icons gleichzeitig sehen — genau dafür wurde das Grid gebaut.
- Slot 6 gehört `LX6R002_UC_Classified_Gear.esm` (Gear-Set-abhängig).
  **Zwei Formen teilen sich diesen einen Slot:** das ausgebrachte Turret und
  sein Rückruf-Item, das an seine Stelle tritt, sobald es geworfen ist. Genau
  deshalb muss der Slot reserviert sein — beide müssen wissen, wo sie liegen.
  Der Slot ist also *nicht* leer, während das Turret steht; leer ist er nur
  kurz zwischen den beiden Schreibvorgängen und solange das Set ausgezogen
  ist. Das Technician-Set bringt drei Items mit; die anderen beiden
  (Frag-Granate, Field Kit) verwaltet Alexander selbst.
