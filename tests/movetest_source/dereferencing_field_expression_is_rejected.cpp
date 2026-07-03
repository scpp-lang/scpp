struct Holder { int x; };
int f() {
    Holder h;
    h.x = 1;
    int y = *h;
    return y;
}
