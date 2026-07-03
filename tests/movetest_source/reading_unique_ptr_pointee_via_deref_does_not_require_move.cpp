int f() {
    std::unique_ptr<int> p = std::make_unique<int>(1);
    int a = *p;
    int b = *p;
    return a + b;
}
