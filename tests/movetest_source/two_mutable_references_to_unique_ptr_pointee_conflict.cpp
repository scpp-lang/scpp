int f() {
    std::unique_ptr<int> p = std::make_unique<int>(1);
    int& r1 = *p;
    int& r2 = *p;
    return r1 + r2;
}
