int f() {
    std::unique_ptr<int> a;
    std::move(a);
    return 0;
}
