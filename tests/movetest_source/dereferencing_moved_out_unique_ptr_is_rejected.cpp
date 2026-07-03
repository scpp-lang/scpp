int f() {
    std::unique_ptr<int> p = std::make_unique<int>(1);
    std::unique_ptr<int> q = std::move(p);
    int x = *p;
    return x;
}
