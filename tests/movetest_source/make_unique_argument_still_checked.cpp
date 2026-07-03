int f() {
    std::unique_ptr<int> x;
    std::unique_ptr<int> a = std::make_unique<int>(x);
    return 0;
}
