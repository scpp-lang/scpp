int f() {
    std::unique_ptr<int> p = std::make_unique<int>(1);
    unsafe {
        std::unique_ptr<int> q = std::move(p);
        std::unique_ptr<int> r = std::move(p);
        return 0;
    }
}
