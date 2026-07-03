int inc(int& x) {
    x = x + 1;
    return 0;
}
int main() {
    int a = 5;
    inc(a);
    print_int(a);
    return 0;
}
