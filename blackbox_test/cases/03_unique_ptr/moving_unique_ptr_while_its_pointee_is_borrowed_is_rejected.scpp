// ch03: "a borrow of *p is recorded against p, so moving (std::move(p))
// ... while that borrow is alive is rejected (it would otherwise
// dangle/use-after-free)."
int main() {
    std::unique_ptr<int> p = std::make_unique<int>(5);
    int& r = *p;
    std::unique_ptr<int> q = std::move(p);
    return r;
}
