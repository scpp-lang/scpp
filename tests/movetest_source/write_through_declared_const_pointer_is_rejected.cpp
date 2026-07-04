safe int f() {
    int x = 1;
    const int* p = &x;
    unsafe {
        *p = 99;
    }
    return 0;
}
