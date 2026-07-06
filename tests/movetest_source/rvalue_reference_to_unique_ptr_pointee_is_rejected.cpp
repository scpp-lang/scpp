// ch05 §5.2/pre-existing v0.1 restriction: a reference's referent may
// never itself be std::unique_ptr (would require the borrow checker to
// also reason about moving/dropping the owner out from under a live
// borrow) -- this applies to `T&&` exactly the same as `T&`/`const T&`,
// so `std::unique_ptr<int>&&` is rejected too, not a loophole around the
// restriction. Use a plain by-value `std::unique_ptr<int>` parameter
// instead (already fully supported).
int take(std::unique_ptr<int>&& p) { return *p; }
int main() {
    std::unique_ptr<int> x = std::make_unique<int>(7);
    return take(std::move(x));
}
