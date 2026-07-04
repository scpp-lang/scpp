int main() {
    int x = 2147483647;
    int y = 0;
    unsafe {
        y = x + 1;
    }
    print_int(y);
    return 0;
}
