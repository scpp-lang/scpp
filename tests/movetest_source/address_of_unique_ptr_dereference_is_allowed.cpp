int f() {
    std::unique_ptr<int> p = std::make_unique<int>(7);
    int* raw = &(*p);
    return 0;
}
