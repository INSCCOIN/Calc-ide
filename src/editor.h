/* editor.h - the full-screen editor. */
#ifndef EDITOR_H
#define EDITOR_H
int editor_run(const char *path);   /* returns to the caller when the user quits */
int editor_open(int n, char **paths); /* several files, one tab each */
#endif
