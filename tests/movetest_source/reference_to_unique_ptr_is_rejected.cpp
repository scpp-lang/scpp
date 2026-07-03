int f() {
    std::unique_ptr<int> p = std::make_unique<int>(5);
    const int& r = p;
    return 0;
}
