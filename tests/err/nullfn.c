// error: call through a null function pointer
int (*fp)(int);
int main(void) { return fp(3); }
