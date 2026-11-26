/* shell.h - the CalcIDE command line. */
#ifndef SHELL_H
#define SHELL_H

#define CALCIDE_VERSION "0.1"

void shell_init(void);
void shell_main(void);                       /* never returns */
int  shell_exec(const char *line);           /* run one command line */
/* compile src to a .cbc next to it; *errline gets the first error line */
int  shell_compile(const char *src, char *out, int *errline);
int  shell_run_program(const char *path, const char *name);

/* files embedded in the firmware (examples, the compiler's own source) */
typedef struct { const char *path; const unsigned char *data; unsigned len; } EmbeddedFile;
extern const EmbeddedFile embedded_files[];
extern const int embedded_count;

#endif
