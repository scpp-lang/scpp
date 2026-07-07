int f(int* p) {
    [[scpp::unsafe]] {
        const int& r = *p;
        *p = 5;
        return r;
    }
}
