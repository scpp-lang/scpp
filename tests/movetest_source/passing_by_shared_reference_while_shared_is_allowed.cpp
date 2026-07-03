int read_it(const int& x) { return x; }
int f() {
    int a = 1;
    const int& r = a;
    read_it(a);
    return 0;
}
