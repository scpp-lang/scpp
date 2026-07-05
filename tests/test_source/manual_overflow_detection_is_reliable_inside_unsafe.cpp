bool add_one_overflows(int x) {
    unsafe {
        return (x + 1) < x;
    }
}
int main() {
    int x = 2147483647;
    print_bool(add_one_overflows(x));
    return 0;
}
