int f() {
    std::unique_ptr<int> p = std::make_unique<int>(1);
    int& r = *p;
    r = 5;
    return *p;
}
