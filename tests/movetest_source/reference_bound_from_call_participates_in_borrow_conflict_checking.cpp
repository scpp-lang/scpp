int& identity(int& a) {
    return a;
}

int f() {
    int x = 10;
    int& r = identity(x);
    int& s = x;
    return r + s;
}
