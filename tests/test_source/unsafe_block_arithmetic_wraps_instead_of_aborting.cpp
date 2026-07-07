int add_one_unsafely(int x) {
    [[scpp::unsafe]] {
        return x + 1;
    }
}
int main() {
    int x = 2147483647;
    print_int(add_one_unsafely(x));
    return 0;
}
