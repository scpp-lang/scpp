import std;
// spec §6.2(4)/§6.4: reassigning a *previously* moved-out variable
// (`b = std::move(a);` after `b` itself was already moved-out earlier,
// via `Resource c(std::move(b));`) must restore it to a normal,
// destructible state -- its destructor must run again at scope-exit
// (not be permanently suppressed by the earlier move).
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
    Resource c(std::move(b));
    b = std::move(a);
    print_int(b.get());
    return 0;
}
