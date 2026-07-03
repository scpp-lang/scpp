int& identity(int& a) {
    return a;
}
int inc(int& v) {
    v = v + 1;
    return 0;
}
int main() {
    int x = 10;
    int& r = identity(x);
    r = 20;
    print_int(x);
    inc(identity(x));
    print_int(x);
    return 0;
}
