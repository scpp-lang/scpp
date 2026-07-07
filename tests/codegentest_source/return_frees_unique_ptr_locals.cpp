import std;
int main() {
    std::unique_ptr<int> a = std::make_unique<int>(1);
    std::unique_ptr<int> b = std::move(a);
    return 0;
}
