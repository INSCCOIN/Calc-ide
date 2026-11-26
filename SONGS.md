# Songs on CalcIDE

CalcIDE has a small 4-channel synthesizer. You can use it three ways:

- **From the shell:** `play C4 E4 G4` plays notes straight away.
- **From a song file:** `play mysong.song` plays a text file, one line per channel.
- **From C:** `play("C4 E4 G4")` and `play("@/path/mysong.song")` work inside your programs.

All three use the same note language, so anything you try at the prompt can go into a song file or a C string unchanged.

**Contents**

- [Quick start](#quick-start)
- [The note language](#the-note-language)
- [Demo 1: writing a song file, step by step](#demo-1-writing-a-song-file-step-by-step)
- [Demo 2: play() from C](#demo-2-play-from-c)
- [Keeping channels in step](#keeping-channels-in-step)
- [Tips for sounding good](#tips-for-sounding-good)
- [Troubleshooting](#troubleshooting)
- [Reference card](#reference-card)

---

## Quick start

Run `install` once. It copies the bundled songs to `/calcide/songs/` and the examples to `/calcide/examples/`.

```
/> install
/> ls /calcide/songs
/> play /calcide/songs/demo.song
/> play /calcide/songs/lavalamp.song
```

`demo.song` plays every instrument and every drum, so it's the best one to try first. The **break key** stops whatever is playing.

Try a few notes yourself:

```
/> play C4 E4 G4 C5
/> play T180 L8 C5 D5 E5 F5 G5 A5 B5 C6/2
/> play K/4 SN/4 K/8 K/8 SN/4
/> play T120 C4/2 G4/2 | ITRI C3/1 | K SN K SN
```

The last one plays a melody, a bass line and a drum beat at once. The `|` separates channels.

### The bundled songs

| File | Style | Length |
|---|---|---|
| `demo.song` | every instrument, then every drum | short |
| `sunrise.song` | the tutorial song built below | 0:36 |
| `beat.song` | drums only: rock, funk, claps, a tom fill | short |
| `ode.song` | Ode to Joy: melody, bass and drums | short |
| `furelise.song` | Für Elise for two triangle-wave "hands" | short |
| `canon.song` | Pachelbel's Canon, three voices entering in turn | short |
| `chiptune.song` | an original 4-channel chiptune | short |
| `lavalamp.song` | *Lava Lamp Dream*: a 70s psychedelic E dorian vamp | 1:20 |
| `tangerine.song` | *Tangerine Sitar*: a raga over a drone | 1:18 |
| `velvetfuzz.song` | *Velvet Fuzz*: a heavy A minor riff | 1:20 |
| `caravan.song` | *Cosmic Caravan*: a G mixolydian groove | 1:23 |
| `mandala.song` | *Electric Mandala*: F lydian, floats then grooves | 1:20 |

Open any of them with `edit /calcide/songs/caravan.song` to see how it's written.

---

## The note language

A tune is a list of **tokens** separated by spaces. Tokens are case-sensitive: notes and commands are capitals.

### Notes

```
C4  D4  E4  F4  G4  A4  B4  C5      a C major scale
C#4 Eb4 F#5 Bb3                     sharps (#) and flats (b)
R                                   a rest (silence)
```

A note is a letter `A`–`G`, then an optional `#` or `b`, then an optional octave from `0` to `8`. Octave 4 is the middle of the piano, and `A4` is 440 Hz. A note with no octave uses the current default (`O`, which starts at 4).

### Lengths

Add a length after a `/`:

| Token | Length | Beats |
|---|---|---|
| `C4/1` | whole note | 4 |
| `C4/2` | half note | 2 |
| `C4/4` | quarter note | 1 |
| `C4/8` | eighth note | ½ |
| `C4/16` | sixteenth | ¼ |
| `C4/32` | thirty-second | ⅛ |
| `C4/4.` | dotted quarter (a dot adds half) | 1½ |

A note with no length uses the default (`L`, which starts at 4, a quarter note). Rests take lengths too: `R/2`.

### Settings

These change how the following notes play. They apply only to the channel they're in.

| Token | What it sets | Default |
|---|---|---|
| `T120` | tempo, in quarter notes per minute (20–600) | 120 |
| `L8` | default length (1, 2, 4, 8, 16, 32) | 4 |
| `O5` | default octave (0–8) | 4 |
| `V7` | volume (0–9) | 7 |
| `ISQ` | instrument: square wave, a bright retro beep | ✓ |
| `IPLS` | instrument: 25% pulse, a thin, nasal lead | |
| `ITRI` | instrument: triangle, soft and round, ideal for bass | |
| `ISAW` | instrument: sawtooth, buzzy like brass or fuzz guitar | |
| `ISIN` | instrument: sine, a pure flute or pad | |

The **first** `T` in a tune sets the tempo for every channel, so put it at the start.

### Drums

Drums are synthesized, so there are no sample files. A drum is a token like any note and takes a length: `K/8`.

| Token | Drum |
|---|---|
| `K` | kick (bass drum) |
| `SN` | snare |
| `HH` | closed hi-hat |
| `OH` | open hi-hat |
| `CP` | hand clap |
| `TH` | high tom |
| `TL` | low tom |
| `CY` | crash cymbal |

Drums can share a channel with notes, but they're easier to read on a channel of their own.

### Repeats

`[ ... ]x4` plays the part in brackets four times. Brackets nest, and `[ ... ]` with no count repeats twice.

```
/> play T140 [K/8 HH/8 SN/8 HH/8]x4
/> play T140 [[C5/16 E5/16 G5/16 E5/16]x2 [D5/16 F5/16 A5/16 F5/16]x2]x2
```

### Channels

Separate up to four channels with `|`. They all start together:

```
/> play T100 E5 D5 C5/2 | ITRI V9 C3/2 G2/2 | K/4 SN/4 K/4 SN/4
```

---

## Demo 1: writing a song file, step by step

We'll build `sunrise.song`, a 36-second, 4-channel song in C major. The finished file is bundled at `/calcide/songs/sunrise.song` if you want to skip ahead.

### Step 1: try a melody at the prompt

Get the tune right before opening the editor:

```
/> play T112 IPLS E5 G5 A5 G5 E5/2 D5/2 C5 D5 E5 G5 D5/1
```

That's four bars of 4 beats each: `E5 G5 A5 G5` (4 quarters), `E5/2 D5/2` (2 halves), `C5 D5 E5 G5`, and `D5/1` (a whole note).

### Step 2: start the file

```
/> edit /sunrise.song
```

Type this, then save with **Ctrl+S**:

```
# Sunrise - my first song
T112
1: IPLS V6
1: E5/4 G5/4 A5/4 G5/4   E5/2 D5/2     C5/4 D5/4 E5/4 G5/4   D5/1
1: E5/4 G5/4 A5/4 C6/4   B5/2 A5/2     G5/4 E5/4 D5/4 E5/4   C5/1
```

How song files work:

- **`1:` to `4:`** at the start of a line say which channel it belongs to. Lines for the same channel are joined one after another, so you can write a part a few bars at a time.
- A line with **no number** goes to channel 1. That's why `T112` alone works: it is the first `T`, so it sets the tempo for every channel.
- **`#`** starts a comment, but only at the start of a line or after a space, because `C#4` is a note.
- Extra spaces don't matter, so line up bars to make them easy to count.

Play it with **Ctrl+Q** to leave the editor, then:

```
/> play /sunrise.song
```

### Step 3: add a bass line

The chords under the melody are C, Am, F, G, C, Em, F–G, C. A triangle wave plays the root and fifth of each chord as half notes. Add these lines:

```
2: ITRI V9
2: C3/2 G2/2  A2/2 E2/2  F2/2 C3/2  G2/2 D3/2
2: C3/2 G2/2  E2/2 B2/2  F2/2 G2/2  C3/1
```

Channel 2 has 8 bars of 4 beats too: every bar is either two halves or one whole note. **Each channel must add up to the same length.** Otherwise one finishes early and the parts drift apart; [Keeping channels in step](#keeping-channels-in-step) explains how to count.

### Step 4: add drums

Use a classic rock beat, with a fill in the last bar to lead back to the start:

```
4: V6
4: [K/4 HH/8 HH/8 SN/4 HH/8 HH/8]x7  SN/8 SN/8 TH/8 TH/8 TL/8 TL/8 K/8 OH/8
```

Each beat pattern is 1 + ½ + ½ + 1 + ½ + ½ = 4 beats. It plays 7 times, then the fill is 8 eighths = 4 beats, so 8 bars in all. That matches the melody.

### Step 5: add a chord arpeggio

A quiet square wave, with `L8` so every note is an eighth, walks up and down each chord. It fills the gap between bass and melody:

```
3: ISQ V3 L8
3: [C4 E4 G4 E4]x2  [A3 C4 E4 C4]x2  [F3 A3 C4 A3]x2  [G3 B3 D4 B3]x2
3: [C4 E4 G4 E4]x2  [E3 G3 B3 G3]x2  F3 A3 C4 A3 G3 B3 D4 B3  [C4 E4 G4 C5]x2
```

### Step 6: repeat it and add an ending

Wrap each channel's 8 bars in `[ ... ]x2` to play the song twice, then end every channel on one long note, with a crash cymbal on the drums. Here is the finished `sunrise.song`:

```
# Sunrise - the tutorial song from SONGS.md
# 1: lead (pulse)   2: bass (triangle)   3: chord arpeggio (square)   4: drums
T112
1: IPLS V6
1: [E5/4 G5/4 A5/4 G5/4   E5/2 D5/2     C5/4 D5/4 E5/4 G5/4   D5/1
1:  E5/4 G5/4 A5/4 C6/4   B5/2 A5/2     G5/4 E5/4 D5/4 E5/4   C5/1]x2
1: C6/1
2: ITRI V9
2: [C3/2 G2/2  A2/2 E2/2  F2/2 C3/2  G2/2 D3/2
2:  C3/2 G2/2  E2/2 B2/2  F2/2 G2/2  C3/1]x2
2: C2/1
3: ISQ V3 L8
3: [[C4 E4 G4 E4]x2  [A3 C4 E4 C4]x2  [F3 A3 C4 A3]x2  [G3 B3 D4 B3]x2
3:  [C4 E4 G4 E4]x2  [E3 G3 B3 G3]x2  F3 A3 C4 A3 G3 B3 D4 B3  [C4 E4 G4 C5]x2]x2
3: C4/1
4: V6
4: [[K/4 HH/8 HH/8 SN/4 HH/8 HH/8]x7  SN/8 SN/8 TH/8 TH/8 TL/8 TL/8 K/8 OH/8]x2
4: CY/1
```

A repeat can start on one line and close on a later line of the same channel, as channel 1 does.

Every channel is 2 × 32 + 4 = **68 beats**. At `T112` one beat lasts 60 / 112 s, so the song runs 68 × 60 / 112 ≈ **36 seconds**.

### Step 7: experiment

- Change `T112` to `T80` for a slow ballad or `T160` for a punk version.
- Swap `IPLS` for `ISAW` (buzzy) or `ISIN` (soft).
- Replace the drum line with `4: [K/8 K/8 HH/8 K/8 SN/8 HH/8 K/8 OH/8]x16` for a busier beat. That's still 4 beats × 16 = 64; add `4: CY/1` for the ending.

---

## Demo 2: play() from C

`play()` is a built-in, so there's nothing to include. It takes the same string you'd type after `play` at the prompt, and it **waits until the music finishes**, then returns 0. It returns -1 if the tune has an error or the song file can't be read.

```c
int play(char *notes);       // a tune, or "@/path.song" for a song file
int play_wav(char *path);    // a .wav file (8/16-bit PCM, up to 48 kHz)
```

### The smallest program

```c
#include <stdio.h>

int main(void)
{
    printf("Charge!\n");
    play("T180 ISQ L8 G4 C5 E5 G5/4 E5 G5/2");
    return 0;
}
```

Save it as `/charge.c` and type `run /charge.c`.

### Playing a song file

Put `@` in front of a path to play a song file:

```c
if (play("@/calcide/songs/sunrise.song") < 0)
    printf("couldn't play it - did you run 'install'?\n");
```

### The full demo: jukebox.c

`examples/jukebox.c` (installed as `/calcide/examples/jukebox.c`) shows everything:

1. it plays a fixed three-channel tune;
2. it **builds a song in memory** with `sprintf` and `strcat`, then plays it at two tempos;
3. it turns the keyboard into a **drum pad**;
4. it offers a **jukebox menu** that plays `.song` files.

```
/> run /calcide/examples/jukebox.c
```

```c
// jukebox.c - everything play() can do from C (the demo in SONGS.md)
//   1. play a fixed tune         2. build a tune at run time
//   3. a drum pad on the keys    4. a jukebox of .song files
#include <stdio.h>
#include <string.h>
#include <picocalc.h>

#define NSONGS 8
char *titles[NSONGS] = {"sunrise", "lavalamp", "tangerine", "velvetfuzz",
                        "caravan", "mandala", "chiptune", "ode"};

char lead[400], bass[200], drums[300], groove[1000];

// Four bars over C - Am - F - G. Each channel must add up to the same
// length: here every bar is 4 beats on every channel.
void build_groove(int tempo)
{
    char *arp[4]  = {"C4 E4 G4 E4", "A3 C4 E4 C4", "F3 A3 C4 A3", "G3 B3 D4 B3"};
    char *root[4] = {"C3", "A2", "F2", "G2"};
    int i;

    sprintf(lead, "T%d IPLS V5 L8 ", tempo);
    strcpy(bass, "ITRI V9 ");
    strcpy(drums, "V6 ");
    for (i = 0; i < 4; i++) {
        sprintf(lead + strlen(lead), "[%s]x2 ", arp[i]);                 // 8 x 1/8
        sprintf(bass + strlen(bass), "%s/4. %s/8 %s/2 ", root[i], root[i], root[i]);
        strcat(drums, i < 3 ? "K/4 HH/8 HH/8 SN/4 HH/8 OH/8 "
                            : "SN/16 SN/16 SN/16 SN/16 TH/8 TH/8 TL/8 TL/8 CY/4 ");
    }
    sprintf(groove, "%s | %s | %s", lead, bass, drums);
}

void drum_pad(void)
{
    int k;
    printf("\nDrum pad: k kick  s snare  h hat  o open hat\n");
    printf("          c clap  t/g toms  x crash   q done\n");
    for (;;) {
        k = getkey();
        switch (k) {
        case 'k': play("K/16"); break;
        case 's': play("SN/16"); break;
        case 'h': play("HH/16"); break;
        case 'o': play("OH/8"); break;
        case 'c': play("CP/16"); break;
        case 't': play("TH/16"); break;
        case 'g': play("TL/16"); break;
        case 'x': play("CY/4"); break;
        case 'q': case KEY_ESC: return;
        }
    }
}

void jukebox(void)
{
    char path[64];
    int i, k;
    for (;;) {
        printf("\nJukebox\n");
        for (i = 0; i < NSONGS; i++) printf("  %d  %s\n", i + 1, titles[i]);
        printf("  q  quit\n> ");
        k = getkey();
        if (k == 'q' || k == KEY_ESC) { printf("bye\n"); return; }
        if (k < '1' || k >= '1' + NSONGS) { printf("?\n"); continue; }
        sprintf(path, "@/calcide/songs/%s.song", titles[k - '1']);
        printf("playing %s\n", path + 1);
        if (play(path) < 0) printf("can't play it - run 'install' first\n");
    }
}

int main(void)
{
    printf("1. A fixed tune, three channels\n");
    play("T140 ISQ L8 C5 E5 G5 C6/4 G5 C6/2 | ITRI V9 C3/2 G2/2 | K/4 SN/4 K/4 CY/4");

    printf("2. A groove built by the program\n");
    build_groove(100);
    printf("   %d characters of notes, slow...\n", strlen(groove));
    play(groove);
    build_groove(150);
    printf("   ...and fast\n");
    play(groove);

    drum_pad();
    jukebox();
    return 0;
}
```

What to notice:

- **Building tunes as strings.** A tune is just text, so `sprintf`, `strcat` and loops can generate one. `build_groove` writes each channel into its own buffer, then joins them with `" | "`. The tempo is a parameter, so the same groove plays slow and then fast.
- **Every bar is 4 beats on every channel.** Lead: 8 × ⅛ notes. Bass: `/4.` + `/8` + `/2` = 1½ + ½ + 2. Drums: 1 + ½ + ½ + 1 + ½ + ½, and the fill is 4 × ¼ + 4 × ½ + 1. Count yours the same way.
- **Short sounds make an instrument.** `play("K/16")` lasts about an eighth of a second, so every key press triggers one drum hit straight away.
- **Check the return value** when playing a file, in case it's missing.
- **Buffers.** The groove is about 300 characters, so the 1000-byte buffer has room to spare. If you generate longer tunes, make the buffers bigger.
- **Break key.** It stops a song that's playing. Pressed while the program waits at `getkey()`, it ends the program.

### More ideas for programs

- **Sound effects in a game:** `play("T600 ISQ L32 C6 G5 E5 C5")` for a falling "lose" sound, and `play("T600 L32 C5 E5 G5 C6")` for a rising "win" one.
- **Make a random tune:** pick notes from a scale array with `rand()`, `strcat` them, and play the result.
- **Mix with beep():** `beep(freq_hz, ms)` plays a single raw frequency. It's handy for sweeps that don't fit the 12 notes of the scale.
- **WAV files:** `play_wav("/music/intro.wav")` plays real recordings. Use 8- or 16-bit PCM, mono or stereo; 8–16 kHz mono keeps files small.

---

## Keeping channels in step

Channels run independently. If one is shorter, it stops early and the others carry on. If a channel is longer in the *middle*, everything after it is out of time. So **count beats per bar** on every channel.

| Length | Beats | Four beats is… |
|---|---|---|
| `/1` | 4 | 1 whole |
| `/2` | 2 | 2 halves |
| `/4` | 1 | 4 quarters |
| `/4.` | 1½ | `/4.` + `/8` + `/2` |
| `/8` | ½ | 8 eighths |
| `/16` | ¼ | 16 sixteenths |

Ways to stay sane:

- Write **one bar per group** with extra spaces between groups, as in `sunrise.song`.
- Put each section in a **repeat**: `[one bar]x8` is easy to count.
- Use **`L8` or `L16`** on busy channels so you don't have to write a length on every note.
- **Song length:** total beats × 60 / tempo = seconds. For example, 128 beats at T96 is 80 s.
- On a PC, `python3 tools/songcheck.py mysong.song` prints each channel's beat count:

  ```
  mysong.song ok  1:68 2:68 3:68 4:68
  ```

---

## Tips for sounding good

- **Volume balance:** keep the lead at `V5`–`V7`, the bass at `V8`–`V9` (triangle is quiet), and pads and arpeggios at `V3`–`V4`. The mixer scales by channel count and soft-limits peaks, so four loud channels won't clip, but they will crowd each other.
- **Bass:** `ITRI` in octaves 2–3 sounds the most like a bass guitar.
- **Leads:** `IPLS` and `ISAW` cut through. `ISIN` is gentle, like a flute.
- **Chords:** with only four channels, play chords as **arpeggios** (`L8` or `L16` notes cycling through the chord) instead of holding three notes.
- **Drums:** `K` on beats 1 and 3, `SN` on 2 and 4, and `HH` eighths in between is the standard rock beat. Put a tom roll (`TH TH TL TL`) in the last bar of a section to lead into the next, then a `CY` on the first beat of the new one.
- **Endings:** finish every channel on a long note (`/1`) with `CY/1` on the drums.

---

## Troubleshooting

| Problem | Cause and fix |
|---|---|
| `play: bad note 'X9'` | A token wasn't understood. Notes and commands are capitals (`C4`, `ISQ`, `SN`), and the octave goes before the length: `C4/8`, not `C/8 4`. |
| A sharp disappears | `#` after a space starts a comment. Write `C#4` with no space before the `#`. |
| Parts drift apart | A channel has the wrong number of beats in a bar. Count with the table above, or run `songcheck.py`. |
| One part ends early | Its channel is shorter. Pad it with rests (`R/1`) or add the missing bars. |
| The whole song is too fast or slow | Only the first `T` sets the starting tempo. Change the `T` line at the top of the file. |
| `play()` returns -1 | The tune has an error, or the file doesn't exist. Check the path; bundled songs need `install`. |
| No sound at all | Check `mute` and the volume (`set volume`). The shell's `beep` command is a quick test. |

---

## Reference card

```
NOTES    C D E F G A B   + # or b   + octave 0-8      C#4 Eb5 G3
REST     R
LENGTH   /1 /2 /4 /8 /16 /32   dotted: /4.            C4/8  R/2  E5/4.
TEMPO    T20..T600   (first T sets every channel)     T120
DEFAULTS L1..L32 length   O0..O8 octave               L8 O5
VOLUME   V0..V9                                       V6
VOICES   ISQ square  IPLS pulse  ITRI triangle  ISAW saw  ISIN sine
DRUMS    K kick  SN snare  HH hat  OH open hat  CP clap
         TH high tom  TL low tom  CY crash
REPEAT   [ ... ]x4   (nests; [ ] alone = twice)
CHANNELS a | b | c | d   (up to 4)
FILES    1: ... 4: lines, joined per channel; '# ' comments
SHELL    play NOTES...     play FILE.song     wav FILE.wav
C        play("NOTES")     play("@/path.song")     play_wav("x.wav")
```

See also [commands.md](commands.md#the-synthesizer) for the shell reference and [compiler.md](compiler.md) for the C library.
