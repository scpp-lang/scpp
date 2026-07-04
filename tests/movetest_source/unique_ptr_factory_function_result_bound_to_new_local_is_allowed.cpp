std::unique_ptr<int> make_it() {
    return std::make_unique<int>(11);
}
int f() {
    std::unique_ptr<int> p = make_it();
    return *p;
}
