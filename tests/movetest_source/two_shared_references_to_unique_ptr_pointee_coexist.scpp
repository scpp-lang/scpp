import std;
int f() {
    std::unique_ptr<int> p = std::make_unique<int>(1);
    const int& r1 = *p;
    const int& r2 = *p;
    return r1 + r2;
}
