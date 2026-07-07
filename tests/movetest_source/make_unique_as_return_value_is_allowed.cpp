import std;
std::unique_ptr<int> f() {
    return std::make_unique<int>(9);
}
