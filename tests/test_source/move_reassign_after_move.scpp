import std;
int main() {
    std::unique_ptr<int> a;
    std::unique_ptr<int> other;
    std::unique_ptr<int> b = std::move(a);
    a = std::move(other);
    std::unique_ptr<int> c = std::move(a);
    return 5;
}
