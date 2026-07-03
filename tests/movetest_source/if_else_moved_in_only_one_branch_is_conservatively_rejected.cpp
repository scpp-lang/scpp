int f(bool cond) {
    std::unique_ptr<int> a;
    std::unique_ptr<int> sink;
    if (cond) {
        sink = std::move(a);
    }
    std::unique_ptr<int> b = std::move(a);
    return 0;
}
