# ClaudeDeck

Host multi-scheda per sessioni di **Claude Code** su Windows: un solo eseguibile Qt6/C++ che apre
ogni sessione `claude` in una pseudo-console (ConPTY), la mostra in una scheda con un emulatore
di terminale interno e tiene d'occhio lo stato di ciascuna.

Niente Windows Terminal, niente PowerShell, niente plugin: solo l'exe e le DLL di Qt.

## Funzioni

- **Schede**: una sessione Claude Code per scheda, ognuna con la sua cartella di progetto.
- **Avvio in una cartella**: dialog con cartelle recenti, `Sfoglia…`, argomenti extra (`--model opus`, …).
- **Ripresa**: `--continue` (ultima conversazione della cartella) o `--resume [id]` (senza id Claude mostra l'elenco).
- **Stato per scheda** (simbolo + colore nel titolo, riepilogo nella status bar):

  | simbolo | stato | come viene rilevato |
  |---|---|---|
  | `◐` blu | al lavoro | Claude sta scrivendo sul terminale (spinner, output) |
  | `○` verde | in attesa di input | nessun output da 1,5 s |
  | `●` arancio | da vedere | Claude ha suonato la campanella (fine turno / richiesta permesso); si spegne quando digiti nella scheda o la porti in primo piano |
  | `✕` grigio | terminata | il processo è uscito (codice riportato nel terminale) |
  | `!` rosso | errore di avvio | comando non trovato, ConPTY assente, … |

- **Broadcast**: la barra in basso invia lo stesso prompt a tutte le sessioni attive (o solo a quelle in attesa),
  come se fosse stato incollato e seguito da Invio.
- Scrollback, selezione col mouse (doppio clic = parola), copia/incolla, zoom, titolo della finestra preso da
  quello che Claude Code imposta via OSC.

## Requisiti

- Windows 10 1809 o successivo (ConPTY). Le API vengono caricate a runtime da `kernel32.dll`, quindi il build
  non dipende dalla versione dell'SDK.
- Qt 6.5+ (MSVC 2022 o MinGW), CMake 3.21+.
- Claude Code installato: viene cercato `claude` nel `PATH` (`claude.exe` dell'installer nativo oppure lo shim
  `claude.cmd` di npm, che viene lanciato tramite `cmd.exe`). Il comando è configurabile in *Sessione → Impostazioni…*.

Il codice compila anche su Linux/macOS con un backend `forkpty()` di appoggio (serve per i test; non è
l'obiettivo del progetto).

## Build

```bat
:: MSVC 2022
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:\Qt\6.8.0\msvc2022_64
cmake --build build --config Release
C:\Qt\6.8.0\msvc2022_64\bin\windeployqt build\Release\claudedeck.exe

:: MinGW (quello fornito da Qt)
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_PREFIX_PATH=C:\Qt\6.8.0\mingw_64 -DCMAKE_BUILD_TYPE=Release
cmake --build build
C:\Qt\6.8.0\mingw_64\bin\windeployqt build\claudedeck.exe
```

Test dell'emulatore (tutte le piattaforme): `cmake -DCLAUDEDECK_BUILD_TESTS=ON …` poi `ctest --test-dir build -C Release`
(o lancia `vt_test` a mano).

## Scorciatoie

| tasti | azione |
|---|---|
| `Ctrl+Shift+T` | nuova sessione (dialog) |
| `Ctrl+Shift+N` | nuova sessione nell'ultima cartella usata |
| `Ctrl+Shift+R` | riavvia la scheda corrente con `--continue` |
| `Ctrl+Shift+W` | chiudi scheda (chiede conferma se il processo è vivo) |
| `Ctrl+Tab` / `Ctrl+Shift+Tab`, `Ctrl+PgDn` / `Ctrl+PgUp` | scheda successiva / precedente |
| `Ctrl+1…9` | vai alla scheda N |
| `Ctrl+Shift+B` | vai alla barra broadcast (`Esc` torna al terminale) |
| `Ctrl+Shift+C` / `Ctrl+Shift+V` | copia / incolla |
| `Ctrl+C` con selezione | copia (senza selezione è il normale `^C`) |
| `Ctrl+V` | incolla testo; se negli appunti c'è solo un'immagine il `^V` va a Claude Code (incolla immagine) |
| tasto destro | copia la selezione, oppure incolla se non c'è selezione |
| `Shift+Invio` | va a capo nel prompt di Claude Code (invia `\` + Invio, la sequenza di `/terminal-setup`) |
| `Alt+Invio` | Meta+Invio (come Option+Invio su macOS) |
| `Shift+PgUp` / `Shift+PgDn`, rotella | scorri lo scrollback |
| `Ctrl+rotella`, `Ctrl+=` / `Ctrl+-` / `Ctrl+0` | zoom |

Tutto il resto (`Ctrl+C`, `Ctrl+R`, `Ctrl+B`, `Esc`, `Shift+Tab`, frecce, …) arriva a Claude Code.

## Campanella = "da vedere"

Lo stato arancione si accende quando il programma nella scheda invia il carattere BEL. Claude Code lo fa se
il canale di notifica è la campanella del terminale:

```
claude config set -g preferredNotifChannel terminal_bell
```

Senza questa impostazione funziona comunque la distinzione blu/verde (lavora / in attesa).

## Architettura

```
src/
  Wcwidth.h          larghezza dei caratteri (0/1/2 celle), stessa classificazione di ConPTY
  VtEmulator.*       emulatore VT100/xterm: parser (C0, ESC, CSI, OSC, DCS), buffer principale +
                     alternativo, scrollback, regione di scorrimento, SGR 16/256/truecolor, DECSC/DECRC,
                     modalità tracciate (DECCKM, DECAWM, DECTCEM, 1049, bracketed paste, DECSCUSR)
  ConPtySession.*    processo figlio in ConPTY: pipe, CreatePseudoConsole via GetProcAddress, thread di
                     lettura, thread di attesa uscita, Job Object (KILL_ON_JOB_CLOSE) per l'intero albero
                     di processi, risoluzione di claude.exe / claude.cmd.  #else: forkpty()
  TerminalWidget.*   QWidget che disegna la griglia (QPainter, run di celle con stesso stile), cursore,
                     selezione, scrollbar; codifica tasti → sequenze VT; incolla con bracketed paste
  SessionTab.*       una scheda = ConPtySession + TerminalWidget + euristica di stato + broadcast
  NewSessionDialog.* cartella, modalità (nuova / --continue / --resume), argomenti extra
  MainWindow.*       QTabWidget, menu e scorciatoie, barra broadcast, status bar, impostazioni (QSettings)
tests/
  vt_test.cpp        unit test dell'emulatore
  smoke_test.cpp     test headless pty+widget (Linux/macOS), salva un PNG del rendering
  interactive_test.cpp  tastiera, prompt bash, schermo alternativo (Linux/macOS)
```

Perché un emulatore interno: ConPTY non consegna testo, consegna sequenze VT (posizionamento del cursore,
cancellazioni, colori) che qualcuno deve interpretare. L'emulatore copre il repertorio che conhost usa
davvero: CUP/CUU/CUD/CUF/CUB, CHA/VPA, EL/ED/ECH/ICH/DCH/IL/DL, DECSTBM, SU/SD, SGR, alt screen, OSC 0/2.
Le query (DA, DSR, …) le risponde conhost stesso, quindi vengono ignorate.

## Limiti noti (v0.1)

- Ridimensionando la finestra le righe non vengono ri-impaginate (come conhost): il contenuto già scritto
  viene troncato/riempito, Claude Code ridisegna comunque la sua interfaccia.
- Nessun `win32-input-mode`: i tasti vanno come sequenze VT classiche (basta per Claude Code).
- Niente mouse reporting verso l'applicazione, niente hyperlink OSC 8, niente immagini.
- Una sola finestra; gli split (pannelli affiancati) sono la prossima cosa da aggiungere.
