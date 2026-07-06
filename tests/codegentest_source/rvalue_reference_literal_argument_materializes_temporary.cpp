// ch03/ch05 §5.11: a `T&&` argument that isn't itself an addressable
// place (here, a plain literal) must be materialized into a fresh stack
// temporary before its address is passed -- exactly like real C++
// binding a reference to a prvalue.
int take(int&& x) { return x; }
int main() { return take(42); }
