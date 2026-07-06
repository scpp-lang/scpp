// ch03/ch05 §5.11: a call whose own return type is itself a reference
// yields a place/alias, not a fresh rvalue (it's a legitimate `T&`/
// `const T&` source instead, see resolve_borrow_source_root) -- so it
// must not satisfy a `T&&` parameter, even though it superficially looks
// like "a call passed directly as an argument".
int& identity(int& r) { return r; }
int take(int&& x) { return x; }
int main() {
    int y = 5;
    return take(identity(y));
}
