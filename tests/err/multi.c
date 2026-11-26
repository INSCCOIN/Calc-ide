// error: compile 3 errors
int main(void) {
    int x = 1
    int y = undefined_var;
    x = y +;
    return x;
}
