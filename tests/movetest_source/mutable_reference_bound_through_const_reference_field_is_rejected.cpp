struct Foo { int x; };
int f(const Foo& p) {
    int& r = p.x;
    return r;
}
int main() { return 0; }
