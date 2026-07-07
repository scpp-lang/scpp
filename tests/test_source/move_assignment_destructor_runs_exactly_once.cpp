// spec §6.4(3)/(5): `b = std::move(a);` (move assignment) releases
// whatever `b` already owned *before* taking `a`'s value (no leak), and
// `a` is left in the moved-out state so only `b`'s destructor runs at
// scope-exit -- exactly once.
class Resource {
public:
    Resource(int v) { this.p = std::make_unique<int>(v); return; }
    ~Resource() { print_int(999); return; }
    int get() { return *this.p; }
private:
    std::unique_ptr<int> p;
};
int main() {
    Resource a(1);
    Resource b(2);
    b = std::move(a);
    print_int(b.get());
    return 0;
}
