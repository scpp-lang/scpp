int main() {
    int x = 2147483647;
    bool overflowed = false;
    unsafe {
        overflowed = (x + 1) < x;
    }
    print_bool(overflowed);
    return 0;
}
