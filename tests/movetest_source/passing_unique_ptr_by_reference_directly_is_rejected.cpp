int inc(int& v) {
    v = v + 1;
    return 0;
}
int f() {
    std::unique_ptr<int> p = std::make_unique<int>(1);
    inc(p);
    return 0;
}
