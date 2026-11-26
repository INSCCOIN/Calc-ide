/* sound.h - melodies from tracker strings, and WAV playback. */
#ifndef SOUND_H
#define SOUND_H
#include <stdint.h>
/* Play "T120 L4 C4 E4 G4/8 R8 C#5/2." - returns 0, -1 on a bad note
 * (play_error() says which), or 1 if stopped by the break key. */
int play_notes(const char *s);
const char *play_error(void);
/* a .song file: lines "N: notes" for channels 1-4, '#' comments */
int play_file(const char *path);
/* Play a PCM .wav (8/16-bit, mono/stereo, any rate up to 48 kHz):
 * 0 ok, 1 stopped, -1 error (play_error()). */
int play_wav(const char *path);
/* Render a tune (or "@/file.song") to an 8-bit 16 kHz mono .wav instead
 * of the speakers: 0 ok, 1 stopped, -1 error. */
int play_to_wav(const char *notes, const char *out_path, uint32_t *bytes);
char *song_load_text(const char *path);
char *song_expand(const char *s);
#endif
