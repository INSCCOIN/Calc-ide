/* cctool.c - host driver for tests: "cctool cc file.c [-o out]" or
 * "cctool run prog.cbc [args]". Paths are inside $CALCIDE_ROOT. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "hal.h"
#include "console.h"
#include "config.h"
#include "vfs.h"
#include "vm.h"
#include "cc.h"

int main(int argc, char **argv)
{
    int rc;
    setenv("CALCIDE_BATCH", "1", 1);
    hal_init();
    conf_defaults();
    vfs_mount();
    con_init();
    if (argc < 3) { fprintf(stderr, "usage: cctool cc|run ...\n"); return 2; }
    if (getenv("CALCIDE_CWD")) snprintf(vfs_cwd, sizeof vfs_cwd, "%s", getenv("CALCIDE_CWD"));
    if (getenv("CALCIDE_VMKB")) g_conf.vm_mem_kb = atoi(getenv("CALCIDE_VMKB"));
    if (strcmp(argv[1], "cc") == 0) {
        rc = cc_compile(argc - 1, argv + 1);
        if (getenv("CC_STATS")) fprintf(stderr, "peak %zu KB\n", cc_peak_bytes() / 1024);
    } else {
        rc = vm_run_file(argv[2], argc - 2, argv + 2);
        if (getenv("VM_STATS")) fprintf(stderr, "instr %u ms %u heap %u stack %u\n", vm_last.instructions, vm_last.ms, vm_last.heap_peak, vm_last.stack_peak);
    }
    con_flush();
    return rc & 255;
}
