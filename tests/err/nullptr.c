// error: null pointer dereference
struct s { int a, b; };
int main(void) { struct s *p = 0; return p->a; }
