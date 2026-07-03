int f() {
    std::unique_ptr<int> a;
    std::unique_ptr<int> sink;
    while (true) {
        sink = std::move(a);
    }
    return 0;
}
