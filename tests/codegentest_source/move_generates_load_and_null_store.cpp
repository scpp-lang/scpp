import std;
int main() {
    std::unique_ptr<int> a;
    std::unique_ptr<int> b = std::move(a);
    return 0;
}
