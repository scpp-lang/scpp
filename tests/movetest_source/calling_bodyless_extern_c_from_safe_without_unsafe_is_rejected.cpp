extern "C" int c_abs(int n);
safe int f() {
    return c_abs(-1);
}
