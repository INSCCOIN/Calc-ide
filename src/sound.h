/* sound.h - melodies from tracker strings, and WAV playback. */
#ifndef SOUND_H
#define SOUND_H
/* Play "T120 L4 C4 E4 G4/8 R8 C#5/2." - returns 0, -1 on a bad note
 * (play_error() says which), or 1 if stopped by the break key. */
int play_notes(const char *s);
const char *play_error(void);
/* Play a PCM .wav (8/16-bit, mono/stereo, any rate up to 48 kHz):
 * 0 ok, 1 stopped, -1 error (play_error()). */
int play_wav(const char *path);
#endif
