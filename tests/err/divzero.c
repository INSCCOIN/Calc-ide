// error: division by zero
int f(int x) { return 10 / x; }
int main(void) { int z = 0; printf("before\n"); return f(z); }
