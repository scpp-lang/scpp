int f() {
    std::unique_ptr<int> p = std::make_unique<int>(1);
    int x = *p;
    return x;
}
