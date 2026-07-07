import std;
int read(std::unique_ptr<int>& p) {
    return *p;
}
int f() {
    std::unique_ptr<int> p = std::make_unique<int>(1);
    return read(p);
}
