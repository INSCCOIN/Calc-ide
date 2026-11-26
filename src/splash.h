/* splash.h - the boot screen (logo or a .bmp from the SD card). */
#ifndef SPLASH_H
#define SPLASH_H
void splash_show(void);              /* honours splash, splash_ms, splash_image */
int  bmp_show(const char *path);     /* draw a .bmp centred on the panel; 0 on failure */
#endif
