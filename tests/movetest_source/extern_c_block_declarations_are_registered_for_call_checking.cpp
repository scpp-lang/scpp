extern "C" {
    int c_abs(int n);
    void c_exit(int code);
}
safe int f() {
    return c_abs(-1);
}
