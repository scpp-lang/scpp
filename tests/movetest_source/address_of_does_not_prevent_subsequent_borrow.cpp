int f() {
    int x = 5;
    int* p = &x;
    const int& r = x;
    return r;
}
