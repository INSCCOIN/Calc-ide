// error: memory access out of bounds
int main(void) { char *p = (char *)0x7FFFFFF0; *p = 1; return 0; }
