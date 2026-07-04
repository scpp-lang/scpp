// ch05 §5.1: "After std::move(x), x enters the moved-out state; reading a
// moved-out value -> error."
int main() {
    std::unique_ptr<int> a = std::make_unique<int>(1);
    std::unique_ptr<int> b = std::move(a);
    return *a;
}
