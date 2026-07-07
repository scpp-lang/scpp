import std;
int f() {
    std::unique_ptr<int> p = std::make_unique<int>(1);
    int& r = *p;
    std::unique_ptr<int> q = std::move(p);
    return r;
}
