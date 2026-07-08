// ch04 §4.2: an ordinary constructor taking a *different* type's rvalue
// reference (not the enclosing class's own -- never a move constructor,
// spec §6.4(1) only forbids that specific shape) continues to resolve
// and check normally via check_constructor_arguments.
class Foo {
public:
    Foo() { return; }
    int get() const { return 7; }
};
class Bar {
public:
    Bar(Foo&& f) { this.v = f.get(); return; }
    int get() const { return this.v; }
private:
    int v;
};
int main() {
    Foo f;
    Bar b(std::move(f));
    return b.get();
}
