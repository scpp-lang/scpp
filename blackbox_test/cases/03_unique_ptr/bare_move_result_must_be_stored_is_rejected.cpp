// A std::move(x) result that is never stored anywhere (bare expression
// statement) can't actually transfer ownership to anything -- scpp
// requires the move hint be used to initialize or assign into a
// std::unique_ptr.
int main() {
    std::unique_ptr<int> a = std::make_unique<int>(1);
    std::move(a);
    return 0;
}
