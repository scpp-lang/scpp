int f(int* p) {
    [[scpp::unsafe]] {
        return *p;
    }
}
