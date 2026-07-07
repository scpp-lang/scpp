int helper() {
    return 1;
}
int f() {
    [[scpp::unsafe]] {
        [[scpp::unsafe]] {
            return helper();
        }
    }
}
