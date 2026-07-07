int f() {
    int a = 1;
    [[scpp::unsafe]] {
        int& r1 = a;
        int& r2 = a;
        return r1 + r2;
    }
}
