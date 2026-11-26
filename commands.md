# CalcIDE command reference

Commands are case-insensitive. Arguments are separated by spaces, and double quotes group words (`echo "two  spaces"`). Paths can be absolute (`/calcide/examples`) or relative to the current directory, and `.` and `..` work as usual.

If a word isn't a command or an alias, CalcIDE looks for a program with that name: `NAME.cbc` in the current directory, then in `/bin`. So `primes` runs `primes.cbc`, like on DOS.

`help` lists every command and `help CMD` shows its usage. Synonyms are listed together below.

## Contents

- [Line editing keys](#line-editing-keys)
- [System](#system)
- [Display and sound](#display-and-sound)
- [SD card and files](#sd-card-and-files)
- [Programming](#programming)
- [The page file: pf](#the-page-file-pf)
- [Shell conveniences](#shell-conveniences)
- [Settings: set](#settings-set)

## Line editing keys

| Key | Action |
|---|---|
| Left / Right, Home / End (Ctrl+A / Ctrl+E) | move in the line |
| Backspace / Del | delete |
| Up / Down | previous / next command from history |
| Tab | complete a command name or a path (press again to list matches) |
| Ctrl+U or Esc | clear the line |
| Ctrl+K | delete to the end of the line |
| Ctrl+C or Brk | cancel the line, or stop a running program |

## System

| Command | What it does |
|---|---|
| `battery` | Battery level in percent with a bar graph, and whether it is charging (read from the keyboard controller). |
| `shutdown [N]` | Power off, after N seconds if given. Any key cancels the countdown. The keyboard controller cuts power a few seconds after the request. |
| `reset [N]`, `restart [N]` | Restart the device, after N seconds if given. |
| `mem`, `free` | Total SRAM, free heap, largest free block, and the memory programs will get. |
| `uptime` | Time since boot. |
| `version`, `ver` | CalcIDE and compiler version, chip and build date. |
| `temp` | The RP2350's (or RP2040's) internal temperature sensor, in °C and °F. |
| `clock [MHZ]` | Show the CPU clock and the three clock settings. With a number, set the base clock (48–300 on RP2350, saved as `cpu_mhz`; 0 restores the chip's default). Hold Esc at power-on to skip a bad setting. |
| `clock idle MHZ` | The clock at the prompt and in the editor (`cpu_mhz_idle`; 0 = the base clock). |
| `clock run MHZ` | The clock while compiling and running programs (`cpu_mhz_run`; 0 = the base clock). |
| `backlight [N] [KBD]` | Screen backlight 0–255, and optionally the keyboard backlight. Saved. |
| `exit` | Leave CalcIDE. The Pico restarts, which with the PicoCalc SD boot launcher installed lands back in its menu. A stand-alone install just restarts CalcIDE. |
| `bye` | Say goodbye and power off. |
| `bootsel` | Reboot into the RP2350/RP2040 USB bootloader to flash a new `.uf2`. |

```
/> battery
Battery: 87%  on battery
[#################...]
/> shutdown 10
Power off in 10 s (any key cancels)
/> clock
CPU clock: 150.000 MHz
```

## Display and sound

| Command | What it does |
|---|---|
| `width [40\|64]` | 40 columns (8×10 font) or 64 columns (5×10 font), always 32 rows. Saved. |
| `font [8x10\|5x10]` | The same switch, named by font. `font dos` and `font small` also work. |
| `cls`, `clear` | Clear the screen. |
| `color FG [BG]` | Default shell colours, 0–15 or names. Saved. |
| `color list` | Show the 16 colours with their numbers and names. |
| `color theme NAME`, `theme NAME` | Set shell and editor colours together: `dos`, `classic`, `turbo`, `green`, `amber`, `paper`, `matrix`, `night`. |
| `cursor [on\|off]` | Show or hide the cursor. `cursor block` / `cursor underline` sets its shape. Saved. |
| `beep [HZ] [MS]` | Play a tone (default 880 Hz, 200 ms). 20–20000 Hz, up to 10 s. |
| `mute [on\|off]` | Mute or unmute all system and program sounds. With no argument it toggles. Saved. |

```
/> width 64
64 x 32
/> color yellow blue
/> theme green
/> beep 440 500
```

## SD card and files

| Command | What it does |
|---|---|
| `ls [-l] [PATH...]` | List a directory, sorted, directories first. Colours: directories blue, `.c`/`.h` green, `.cbc` yellow. |
| `dir [PATH]` | Long listing with sizes (same as `ls -l`). |
| `cd [DIR]` | Change directory. With no argument, go to `home` from the settings. |
| `pwd` | Print the current directory. |
| `mkdir [-p] DIR...`, `md` | Make directories; `-p` creates parents as needed. |
| `rmdir DIR`, `rd` | Remove an empty directory. |
| `rm [-r] PATH...`, `del` | Remove files; `-r` removes directories and everything in them. |
| `cp FROM TO`, `copy` | Copy a file. If TO is a directory, the file keeps its name. |
| `mv FROM TO`, `ren` | Move or rename a file or directory (replaces an existing file). |
| `cat FILE...`, `type` | Print files. |
| `more FILE` | Print a file one screen at a time (any key: next page, q: stop). |
| `touch FILE` | Create an empty file. |
| `hex FILE` | Hex dump. |
| `df` | Total, used and free space on the SD card. |
| `tree [DIR]` | Directory tree with file sizes, the totals, and SD card usage. |
| `sd`, `sdcard` | Card information: present, SDHC/SDSC, mounted, file system (FAT16/FAT32/exFAT), label, size, free. |
| `mount` | Mount the card (after swapping cards) and reload `/CalcIDE.conf`. |
| `unmount`, `umount` | Flush and unmount, so the card can be removed safely. |
| `sync` | Flush file system buffers to the card. |

```
/> mkdir -p /projects/game
/> cp /calcide/examples/life.c /projects/game
/> cd /projects/game
/projects/game> ls -l
    1275  life.c
1 entries, 1275 bytes
/projects/game> tree /projects
/projects
`-- game/
    `-- life.c 1275
1 directories, 1 files, 2 KB
```

## Programming

| Command | What it does |
|---|---|
| `edit [FILE...]`, `ed` | Open the editor (see the README for its keys). Several files open as tabs: F6/F7 switch, Ctrl+W closes. F5 inside it compiles and runs. |
| `cc FILE.c` | Compile to `FILE.cbc` next to the source. Errors show `file:line: error: message`. |
| `cc FILE.c -o OUT.cbc [-v]` | Choose the output; `-v` prints sizes and the compiler's memory use. |
| `run FILE.c [ARGS]` | Compile, then run. |
| `run FILE[.cbc] [ARGS]` | Run a compiled program. Arguments go to `main(argc, argv)`. |
| `run pf:NAME [ARGS]` | Run a program stored in the page file, directly from flash. |
| `NAME [ARGS]` | Run `NAME.cbc` from the current directory or `/bin`. |
| `install [-f]` | Write the examples to `/calcide/examples`, the compiler source to `/calcide/cc`, and create `/bin`. Existing files are kept unless you give `-f`. |

A program that ends with a non-zero code shows `[exit code N]`. With `set show_stats 1`, every program ends with a summary instead:

```
[48211 instr, 12 ms, heap 3 KB, stack 152 B, exit 0]
```

A compile error shows the line with a caret under the problem. Up to 10 errors are reported at once:

```
multi.c:3:13: error: expected ';' at the end of the line
    3|     int x = 1
     |             ^
multi.c:5:12: error: expected expression
    5|     x = y +;
     |            ^
2 errors
```

A runtime error shows what happened and where:

```
Runtime error: division by zero
  at /err.c:1 in divide()
[exit code -1]
```

## The page file: pf

768 KB of on-board flash for named objects. Names are up to 39 characters, and `/` makes folders.

| Command | What it does |
|---|---|
| `pf set NAME VALUE...` | Store a text value. |
| `pf store FILE [NAME]` | Copy a file from the SD card into flash. The kind (`text`, `data`, `program`) is detected. |
| `pf get NAME FILE` | Copy an object back to the SD card. |
| `pf view NAME` | Print a text object. |
| `pf dump NAME` | Hex dump of any object. |
| `pf list` | Name, kind, size and pages of every object, plus free space. |
| `pf tree` | Objects as a tree, using `/` in names as folders. |
| `pf free` | Page file size, used and free space. |
| `pf wipe NAME` | Delete one object. |
| `pf format` | Erase the whole page file (asks for confirmation). |
| `pf run NAME [ARGS]` | Run a stored program (same as `run pf:NAME`). |

```
/> pf set notes/todo buy milk
/> pf store /bin/hi.cbc tools/hi
stored tools/hi (180 bytes)
/> pf tree
pf:
|-- notes/
|   |-- todo (var, 8)
|-- tools/
|   |-- hi (program, 180)
/> pf free
page file: 768 KB (192 pages of 4 KB)
 used   12 KB  (2 objects, directory 4 KB)
 free  756 KB
```

## Sound and hardware

| Command | What it does |
|---|---|
| `play NOTES...` | Play a tune. Notes are `C D E F G A B` with `#` or `b` and an octave (`C#4`, `Eb5`; the default octave is 4). `/N` sets the length (`/1 /2 /4 /8 /16 /32`, default `/4`), `.` makes a note dotted, `R` is a rest, `T140` sets the tempo, `L8` the default length and `O5` the default octave. The break key stops it. See the synthesizer reference below for channels, instruments and drums. |
| `play FILE.song` | Play a song file (e.g. `play /calcide/songs/chiptune.song`). |
| `play ... -o OUT.wav` | Render notes or a `.song` to a WAV file (8-bit mono, 16 kHz) instead of the speakers: `play /calcide/songs/caravan.song -o /caravan.wav`. |
| `wav FILE.wav` | Play an 8- or 16-bit PCM WAV file, mono or stereo, up to 48 kHz. |
| `tracker [FILE.song]` | A step editor for songs (see *The tracker* below). With no file, it edits `untitled.song`. |
| `xfer` | Copy files to and from a PC over USB until a key is pressed (see *USB file transfer* below). |
| `gpio` | List the GPIO pins programs may use: 0–5, 8, 9, 20–22 and 28. |
| `gpio PIN` | Read a pin. |
| `gpio PIN in\|out\|up\|down` | Set a pin's mode (`up`/`down` are inputs with a pull resistor). |
| `gpio PIN 0\|1` | Make a pin an output and set it. |
| `selftest [quick] [flash]` | Check RAM, the SD card (write, read back, speed), the page file, the compiler and VM (compiles and runs a test program), battery, temperature and clock. Then it shows colour bars and box lines, waits for a key, and plays three notes, asking y/n after each. `quick` skips the interactive part; `flash` also writes and erases a page-file object. |

### The synthesizer

Up to four channels play at once, separated by `|`, and they are mixed at 16 kHz. [SONGS.md](SONGS.md) is a full tutorial on writing songs and playing them from C.

| Token | Meaning |
|---|---|
| `C4` `D#5` `Eb3` | a note (A4 = 440 Hz) |
| `/1 /2 /4 /8 /16 /32`, `.` | length, dotted |
| `R` | rest |
| `K` `SN` `HH` `OH` `CP` `TH` `TL` `CY` | kick, snare, closed hi-hat, open hi-hat, clap, high tom, low tom, crash; they take lengths too: `HH/8` |
| `ISQ` `IPLS` `ITRI` `ISAW` `ISIN` | instrument: square, 25% pulse, triangle, saw, sine (default square) |
| `V0`..`V9` | channel volume (default 7) |
| `T120` | tempo in quarter notes per minute; the first `T` in the string sets the start for every channel |
| `L8`, `O5` | default length, default octave |
| `[ ... ]x3` | repeat (nests; `[ ]` alone repeats twice) |
| `AT120` `RL400` | attack and release time in ms (`AT0 RL0` = the default short, click-free envelope) |
| `VB0`..`VB9` | vibrato depth, about 5.5 Hz (`VB0` = off) |
| `C4~G4/2` | slide from C4 to G4 over the note's length |
| `TP+5` `TP-12` `TP0` | transpose the channel's following notes (not drums) by semitones |
| `C4/16*3` | a length times N (three sixteenths); also `R/1*4` |
| `@/path.song` | (from C) play a song file |

A `.song` file is plain text. `#` starts a comment, but only at the start of a line or after a space, since `C#` is a note. A line `1: ...` to `4: ...` adds to that channel, and a line without a number goes to channel 1. Lines for the same channel run on, one after another:

```
# my song
T120
1: ISQ O5 [E E F G G F E D]x2
2: ITRI V9 [C3/2 G2/2]x4
3: [K/4 HH/8 HH/8 SN/4 HH/8 HH/8]x4
```

```
/> play T160 E5/8 D#5/8 E5/8 D#5/8 E5/8 B4/8 D5/8 C5/8 A4/4.
/> play T120 C4 E4 G4 C5 | K/4 SN/4 K/4 SN/4
/> play /calcide/songs/ode.song
/> wav /music/intro.wav
/> gpio 2 1
GP2 = 1
/> selftest quick
CalcIDE 0.3 self test
memory     PASS  64 KB tested, 412 KB free, largest 398 KB
sd card    PASS  FAT32 32 KB verified, write 480 KB/s, read 1100 KB/s
...
```

### The tracker

`tracker FILE.song` edits a song as a grid: four channel columns, with one row per sixteenth note (16 rows per bar). A cell is a note, a drum, `---` (note off), or empty. An empty cell holds whatever is above it, so a C4 followed by three empty rows is a quarter note. Saving writes a normal `.song` file. The song ends at the end of the last bar that has something in it.

| Key | Action |
|---|---|
| `a`–`g` | a note at the octave shown in the title bar |
| `#` | raise the note a semitone |
| `+` / `-` | octave up / down (the cell, and the default for new notes) |
| `k s h o p t l y` | kick, snare, hi-hat, open hat, clap, high tom, low tom, crash |
| `r` | note off (a rest) |
| Space, Del, Backspace | clear the cell (Space also moves down) |
| `n` | step: how far the cursor moves after a note (1, 2, 4, 8 rows) |
| `i`, `[` `]` | the channel's instrument; its volume down / up |
| `{` `}` | tempo down / up by 5 |
| Arrows, Tab, PgUp/PgDn, Home/End | move (PgUp/PgDn move a bar) |
| Ins / Ctrl+K | insert / delete a row in this channel |
| Ctrl+D | copy this bar to the next bar |
| Enter | play from the start of this bar; F5 or Ctrl+R plays it all; break stops |
| Ctrl+S | save; Ctrl+W saves and also renders `FILE.wav` |
| F1 | help; Esc or Ctrl+Q quits (asks to save) |

Notes preview as you type them. Any `.song` opens if its notes fit the sixteenth-note grid (no `/32`). Slides, effects and tempo changes are dropped, and you're told when that happens.

### USB file transfer

`xfer` turns the USB serial port into a file server. Plug the PicoCalc into a PC and type `xfer` on it. Then, on the PC (Python 3; on Windows install `pyserial` first):

```
python3 tools/calcxfer.py /dev/ttyACM0 ls /
python3 tools/calcxfer.py /dev/ttyACM0 put game.c /code/
python3 tools/calcxfer.py /dev/ttyACM0 get /caravan.wav
python3 tools/calcxfer.py /dev/ttyACM0 mkdir /music put a.song /music/ put b.song /music/
python3 tools/calcxfer.py /dev/ttyACM0 bye
```

The port is `/dev/ttyACM0` on Linux, `/dev/cu.usbmodem…` on macOS and `COM3` (or similar) on Windows. Every file is checked with a CRC-32. A destination ending in `/` keeps the file's name. Pressing any key on the PicoCalc, or `bye`, ends transfer mode. While it is on, the screen shows each file sent or received.

## Shell conveniences

| Command | What it does |
|---|---|
| `help [CMD]` | List commands, or show one command's usage. |
| `history [-c]` | Numbered command history; `-c` clears it, including the saved copy. History is saved to `/calcide/history.txt` (at most 16 KB, oldest first to go) and reloaded at boot; `set history_save 0` turns that off. |
| `!!`, `!N` | Repeat the last command, or command number N. |
| `alias` | List aliases. |
| `alias NAME=COMMAND` | Define an alias (saved in `/CalcIDE.conf`). Words typed after the alias are appended. |
| `alias NAME COMMAND...` | The same, without `=`. |
| `unalias NAME` | Remove an alias. |
| `echo TEXT` | Print text. |
| `sleep [S]` | Wait S seconds (Ctrl+C stops). |

```
/> alias ll=ls -l
/> alias ex=cd /calcide/examples
/> ex
/calcide/examples> ll
```

## Settings: set

| Command | What it does |
|---|---|
| `set` | List every setting and its value. |
| `set KEY VALUE`, `set KEY=VALUE` | Change a setting, apply it, and save `/CalcIDE.conf`. |
| `set save` | Write the file now. |
| `set reload` | Re-read the file (after editing it by hand). |
| `set defaults` | Back to the factory settings. |

The keys are listed in the README under *Settings: CalcIDE.conf*. Some examples:

```
/> set tabsize 2
/> set keyword_color lightred
/> set autoexec ex
/> set prompt $p$g
/> set vm_mem_kb 128
/> set break_key esc
/> set splash_image /pics/boot.bmp
/> set splash_ms 5000
/> set max_files 6
/> set show_stats 1
```
