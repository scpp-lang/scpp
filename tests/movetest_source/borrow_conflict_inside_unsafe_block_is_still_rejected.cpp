safe int f() {
    int a = 1;
    unsafe {
        int& r1 = a;
        int& r2 = a;
        return r1 + r2;
    }
}
