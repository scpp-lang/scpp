int f(int* p) {
    [[scpp::unsafe]] {
        const int& r = *p;
        return r;
    }
}
