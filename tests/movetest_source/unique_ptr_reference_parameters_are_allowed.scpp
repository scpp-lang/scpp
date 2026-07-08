import std;
int take_ref(std::unique_ptr<int>& p) { return *p; }
int take_rref(std::unique_ptr<int>&& p) { return *p; }
int main() {
    std::unique_ptr<int> x = std::make_unique<int>(7);
    std::unique_ptr<int> y = std::make_unique<int>(9);
    return take_ref(x) + take_rref(std::move(y));
}
