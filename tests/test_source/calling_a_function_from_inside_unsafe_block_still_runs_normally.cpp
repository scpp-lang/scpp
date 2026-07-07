int helper() {
    print_int(7);
    return 0;
}
int f() {
    [[scpp::unsafe]] {
        return helper();
    }
}
int main() {
    return f();
}
