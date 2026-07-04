int f() {
    int x = 5;
    const int& r = x;
    int* p = &x;
    return *p + r;
}
