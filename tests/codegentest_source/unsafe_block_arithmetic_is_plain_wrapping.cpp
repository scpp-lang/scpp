int f(int a, int b) {
    int c = 0;
    unsafe {
        c = a + b;
    }
    return c;
}
