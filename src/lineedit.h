/* lineedit.h - the command line editor: cursor keys, history, completion. */
#ifndef LINEEDIT_H
#define LINEEDIT_H

#define LE_HIST_MAX 64

typedef struct {
    char *lines[LE_HIST_MAX];
    int   count;
    int   max;
} LeHistory;

/* Completion callback: given the line and the cursor position, writes the
 * completed line into out (cap bytes) and returns the new cursor position,
 * or -1 for no change. It may print a list of candidates itself. */
typedef int (*LeComplete)(const char *line, int pos, char *out, int cap);
extern LeComplete le_complete;

/* Returns the length of the line, or -1 if it was cancelled (Ctrl+C/ESC on
 * an empty line). The prompt is printed first. */
int  le_readline(char *buf, int max, const char *prompt, LeHistory *h);
void le_history_add(LeHistory *h, const char *line);
void le_history_clear(LeHistory *h);

#endif
