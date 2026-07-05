// ch05 §5.1: `std::move(x)` marks `x` moved-out and transfers ownership;
// the new owner can be dereferenced normally.
int main() {
    std::unique_ptr<int> a = std::make_unique<int>(9);
    std::unique_ptr<int> b = std::move(a);
    return *b;
}
