extern "C" int c_abs(int n);
int f() {
    unsafe {
        return c_abs(-1);
    }
}
