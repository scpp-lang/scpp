const int& read_only(int& a) {
    return a;
}

int f() {
    int x = 10;
    return read_only(x);
}
