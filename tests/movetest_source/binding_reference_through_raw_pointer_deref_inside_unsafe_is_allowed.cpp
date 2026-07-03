int f(int* p) {
    unsafe {
        const int& r = *p;
        return r;
    }
}
