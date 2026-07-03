int f(int* p) {
    unsafe {
        const int& r = *p;
        *p = 5;
        return r;
    }
}
