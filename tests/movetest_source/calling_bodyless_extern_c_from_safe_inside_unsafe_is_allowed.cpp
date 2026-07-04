extern "C" int c_abs(int n);
safe int f() {
    unsafe {
        return c_abs(-1);
    }
}
