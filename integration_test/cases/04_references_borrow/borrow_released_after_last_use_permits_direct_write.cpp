// ch05 §5.3 (NLL): "a reference local's borrow is released right after
// its last use, rather than only at the end of its lexical scope ...
// e.g. a place can be borrowed again immediately after its previous
// borrow's last use, even before the enclosing block ends." Here, `r1`'s
// last use is `int y = r1;`; the direct write `x = 99;` immediately
// afterward (still inside `f`'s body, long before `r1` would go out of
// lexical scope) must be allowed.
safe int f() {
    int x = 5;
    int& r1 = x;
    int y = r1;
    x = 99;
    return x + y;
}

int main() {
    return f();
}
