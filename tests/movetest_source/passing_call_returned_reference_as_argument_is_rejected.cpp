int& identity(int& a) {
    return a;
}

int inc(int& v) {
    v = v + 1;
    return 0;
}

int f() {
    int x = 10;
    return inc(identity(x));
}
