import std;
int f(bool cond) {
    std::unique_ptr<int> a;
    std::unique_ptr<int> sink1;
    std::unique_ptr<int> sink2;
    if (cond) {
        sink1 = std::move(a);
    } else {
        sink2 = std::move(a);
    }
    std::unique_ptr<int> b = std::move(a);
    return 0;
}
