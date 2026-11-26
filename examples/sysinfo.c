// sysinfo.c - the PicoCalc hardware from C
#include <stdio.h>
#include <picocalc.h>

int main(void)
{
    int bat = battery();
    setcolor(BRIGHT_CYAN, BLACK);
    printf("PicoCalc system info\n");
    setcolor(WHITE, BLACK);
    printf("CPU clock   : %d MHz\n", cpu_mhz());
    printf("Temperature : %.1f C\n", temperature());
    if (bat >= 0) printf("Battery     : %d%%%s\n", bat, battery_charging() ? " (charging)" : "");
    printf("Screen      : %d x %d\n", screen_cols(), screen_rows());
    printf("Uptime      : %d s\n", time(0));
    printf("Heap free   : %d bytes\n", heap_free());
    return 0;
}
