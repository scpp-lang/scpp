// ch05 §5.4's universal zero-initialization rule applies to
// std::make_unique<T>() called with zero arguments too: the pointee is
// zero-initialized rather than left undefined.
int main() {
    std::unique_ptr<int> p = std::make_unique<int>();
    return *p;
}
