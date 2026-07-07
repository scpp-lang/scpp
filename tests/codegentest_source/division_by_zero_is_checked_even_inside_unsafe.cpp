int f(int a, int b) {
    [[scpp::unsafe]] {
        return a / b;
    }
}
