/* vm.h - the CalcIDE bytecode virtual machine. */
#ifndef VM_H
#define VM_H

#include <stdint.h>

/* Load a .cbc image (from the SD card, or from the page file when the path
 * starts with "pf:") and run it. argv[0] is the program name. Returns the
 * program's exit code, or -1 on a load or runtime error (already reported). */
int vm_run_file(const char *path, int argc, char **argv);
int vm_run_image(const uint8_t *img, int len, const char *name, int argc, char **argv);

/* statistics from the last run */
typedef struct {
    uint32_t instructions;
    uint32_t ms;
    uint32_t mem_size;
    uint32_t heap_peak;
    uint32_t stack_peak;
} VmStats;
extern VmStats vm_last;

#endif
