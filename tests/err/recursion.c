// error: stack overflow
int deep(int n) { int pad[16]; pad[0] = n; return deep(n + 1) + pad[0]; }
int main(void) { return deep(0); }
