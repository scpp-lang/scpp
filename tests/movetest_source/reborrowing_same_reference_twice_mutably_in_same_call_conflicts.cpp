int add_into(int& dst, int& src) {
    dst = dst + src;
    return 0;
}

int f() {
    int a = 1;
    int& r = a;
    return add_into(r, r);
}
