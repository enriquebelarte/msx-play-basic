# MSX Play Sound

A C implementation mimicking the classic **MSX BASIC `PLAY`** command and **General Instrument AY-3-8910** Programmable Sound Generator (PSG) found in vintage MSX computers.

It parses standard Music Macro Language (MML) strings, supports 3-channel polyphony, emulates the AY-3-8910 square-wave synthesis with an analog RC low-pass filter and logarithmic DAC volume curves, persists musical settings across invocations, and includes an interactive REPL shell and system BEEP reset.

---

## Nostalgia and Origins

- Back in the 1980s, when I was just ten years old, my Canon V-20 MSX computer was my window into a world of pure magic. I spent endless afternoons hunched over real sheet music, patiently translating every note, rest, and tempo into the syntax of the PLAY command. Hearing those paper melodies finally hum to life through the computer's sound chip was an unforgettable thrill—one that sparked a lifelong fascination with both music and code.

## Features

- **3-Channel Polyphony**: Emulates the 3 tone channels (A, B, C) of the AY-3-8910 PSG chip.
- **Authentic MSX Sound Synthesis**:
  - 50% duty-cycle square wave tone generation.
  - Continuous phase accumulation to eliminate audio clicks/pops across note transitions.
  - Analog 1-pole RC low-pass filter (~8 kHz cutoff) reproducing real MSX hardware warm audio output.
  - 16-level logarithmic DAC volume attenuation curve matching the AY-3-8910 hardware.
  - Optional `--sine` switch for pure sine-wave synthesis.
- **Full MSX MML Command Support**:
  - `O`: Octave selection (0 to 8, default 4) along with `<` and `>` relative shifts.
  - `A`–`G`: Standard notes with accidentals (`+` / `#` sharp, `-` flat), per-note durations, and prolongation dots (`.`).
  - `N`: Direct note numbers (`N0` to `N95`, where `N0` = rest, `N1` = C1, `N37` = C4, `N46` = A4).
  - `L`: Default note length (1 to 64, default 4).
  - `T`: Tempo in quarter notes per minute (32 to 255, default 120).
  - `V`: Volume level (0 to 15, default 8).
  - `M`: Music style articulation (`MN` Normal, `MB`/`MS` Background/Staccato, `ML` Legato).
  - `R`: Rests with optional durations and dots.
- **Persistent Settings & System BEEP**:
  - Channel attributes (`O`, `L`, `T`, `V`, `M`) persist across commands until changed or reset.
  - Classic MSX BIOS 1200 Hz `BEEP` restores all default parameters (`O4`, `L4`, `T120`, `V8`, `MN`).
- **Interactive REPL**: A command-line shell mimicking the MSX BASIC prompt.
- **Song Library & `.mus` File Support**: Play multi-channel songs directly from `.mus` text files (`./msx_play songs/jump.mus`).
- **WAV Export**: Direct output to 16-bit 44.1 kHz PCM WAV files (`-o filename.wav`).

---

## MML Command Reference

| Command | Parameter / Range | Default | Description |
| :--- | :--- | :--- | :--- |
| **`O`** | `0` to `8` | `4` | Sets the active octave. |
| **`<` / `>`** | None | — | Decrements (`<`) or increments (`>`) the current octave. |
| **`A`–`G`** | Note name | — | Plays a note. Accepts `+` or `#` (sharp), `-` (flat), optional length (e.g. `C8`, `D16`), and dots (`.`). |
| **`N`** | `N0` to `N95` | — | Plays a note by direct number. `N0` = rest, `N1` = C1 (~32.7 Hz), `N37` = C4 (middle C), `N46` = A4 (440 Hz). |
| **`R`** | `1` to `64` | `L` | Rest (silence). Accepts optional length and dots (e.g. `R4.`). |
| **`L`** | `1` to `64` | `4` | Sets the default note duration (1 = whole note, 4 = quarter note, 8 = eighth note, etc.). |
| **`T`** | `32` to `255` | `120` | Sets tempo in quarter notes per minute. |
| **`V`** | `0` to `15` | `8` | Sets volume level (attenuated through the AY-3-8910 DAC curve). |
| **`M`** | `MN` / `MB` / `ML` | `MN` | Sets music articulation style:<br>• `MN`: **Normal** (7/8 sound, 1/8 silence gap)<br>• `MB`: **Background / Detached** (3/4 sound, 1/4 silence gap, also accepts `MS`)<br>• `ML`: **Legato** (8/8 sound, continuous) |
| **`.`** | Prolongation dot | — | Increases note or rest duration by 50% (multiple dots can be stacked). |

---

## Building and Installing

### Prerequisites
- A standard C99 compiler (`gcc` or `clang`) with standard math library (`-lm`).
- Any Linux audio player: `aplay` (ALSA), `paplay` (PulseAudio), `pw-play` (PipeWire), or `play` (SoX).

### Compilation
Build the binary with `make`:

```bash
make
```

### Run Tests
```bash
make test
```

### Installation
```bash
sudo make install PREFIX=/usr/local
```

---

## Usage

```bash
./msx_play [options] ["Channel 1"] ["Channel 2"] ["Channel 3"]
```

### Command-Line Options

| Option | Description |
| :--- | :--- |
| `-i`, `--interactive` | Starts the interactive MSX PLAY console (REPL). |
| `-b`, `--beep` | Plays the MSX BIOS system BEEP (1200 Hz tone) and resets settings to defaults. |
| `-r`, `--reset` | Resets channel settings to defaults without sounding the beep. |
| `-s`, `--status` | Displays the current persisted settings for all 3 channels. |
| `-o <file.wav>` | Exports mixed audio to a WAV file instead of playing it. |
| `--clean` | Ignores persisted settings and starts from factory defaults. |
| `--sine` | Generates pure sine waves instead of AY-3-8910 PSG square waves. |
| `-h`, `--help` | Displays command-line help and MML syntax overview. |

---

## Examples

### 1. Three-Channel Harmony
```bash
./msx_play "T140 O4 V15 MN C4 D4 E4 F4 G2" "O3 V12 C4 E4 G2" "O2 V10 C2 G2"
```

### 2. Legato Scale with Octave Shifts
```bash
./msx_play "ML O4 C D E F G A B > C < B A G F E D C"
```

### 3. Direct Note Numbers
```bash
./msx_play "T150 N37 N39 N41 N42 N44 N46 N48 N49"
```

### 4. Background / Detached Articulation (Staccato)
```bash
./msx_play "MB O4 C8 D8 E8 F8 G8 A8 B8 > C8"
```

### 5. Playing from a `.mus` Song File
```bash
./msx_play songs/jump.mus
```

### 6. Saving a Song to WAV File
```bash
./msx_play -o jump.wav songs/jump.mus
```

### 7. Trigger MSX System Beep & Reset
```bash
./msx_play --beep
```

---

## Song Library (`songs/`)

The `songs/` directory stores ready-to-play music files using the `.mus` format.

### Supported `.mus` File Formats

A `.mus` file is a plain text file structured in one of two ways:

#### 1. Multi-Line Channel Format (Recommended)
Lines starting with `#` or `;` are comments. Blank lines are ignored. Each non-empty line represents one PSG channel in order (Channel 1, Channel 2, Channel 3):

```text
# Van Halen - Jump (Synthesizer Intro)
# Channel 1: Lead (Top Voice)
T120 V15 O5 R4 D8 R4 E8 R4 C8 R4 C8 R8 D8 R8 D4. E8 R4 C8 R8 O4 A4 G4 G8 ML G2

# Channel 2: Middle Voice
T120 V13 O4 R4 B8 R4 O5 C8 R4 O4 A8 R4 A8 R8 B8 R8 B4. O5 C8 R4 O4 A8 R8 F4 E4 D8 ML D2

# Channel 3: Bottom Voice
T120 V12 O4 R4 G8 R4 G8 R4 F8 R4 F8 R8 G8 R8 G4. G8 R4 F8 R8 C4 C4 C8 ML C2
```

#### 2. Single-Line MSX BASIC Statement
You can also write standard MSX BASIC `PLAY` statements directly:

```text
PLAY "T120 O5 C D E", "O4 E G > C", "O3 C G > C"
```

### Included Songs

- **[`songs/jump.mus`](file:///home/enrique/Proyectos/msx-play-sound/songs/jump.mus)**: The iconic synthesizer intro of Van Halen's *"Jump"*, polyphonically voiced across all 3 PSG channels with syncopated chords and sustained legato ties.

---

## Interactive REPL

Running `./msx_play` without arguments or with `./msx_play -i` enters interactive mode:

```text
========================================================
     MSX PLAY - MSX BASIC PSG Synthesizer (AY-3-8910)   
========================================================
Available commands:
  PLAY "ch1" [, "ch2", "ch3"]  - Play up to 3 channels
  "ch1" [, "ch2"]              - Shorthand notation
  BEEP                          - Sound system beep and reset settings
  STATUS                        - Display current channel settings
  RESET                         - Reset settings without sound
  HELP                          - Show MML syntax reference
  QUIT / EXIT                   - Exit interactive mode
--------------------------------------------------------
Note: Settings (O, L, T, V, M) persist between executions.

MSX> PLAY "T140 O4 V15 MN C D E F G A B > C"
MSX> "O3 V12 C E G", "O4 V15 E G > C"
MSX> STATUS
MSX> BEEP
MSX> QUIT
```

---

## License

MIT License. Feel free to use, modify, and distribute.
