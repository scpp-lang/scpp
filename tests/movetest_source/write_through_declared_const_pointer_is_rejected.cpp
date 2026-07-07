int f() {
    int x = 1;
    const int* p = &x;
    [[scpp::unsafe]] {
        *p = 99;
    }
    return 0;
}
