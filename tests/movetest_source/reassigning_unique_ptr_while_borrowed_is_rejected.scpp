import std;
int f() {
    std::unique_ptr<int> p = std::make_unique<int>(1);
    const int& r = *p;
    p = std::make_unique<int>(2);
    return r;
}
