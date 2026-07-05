// ch05 §5.10: "the only two outcomes are 'exactly one candidate matches'
// or 'zero candidates match' (a compile error requiring an explicit cast
// at the call site, same as any other type mismatch)." `char` matches
// neither f(int) nor f(bool) exactly, so the call is rejected rather than
// silently converting.
safe int f(int x) {
    return x;
}

safe int f(bool b) {
    return 1;
}

safe int g() {
    char c = 'a';
    return f(c);
}

int main() {
    return g();
}
