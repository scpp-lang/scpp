safe int f(char* p) {
    unsafe {
        return *p == 'a';
    }
}
