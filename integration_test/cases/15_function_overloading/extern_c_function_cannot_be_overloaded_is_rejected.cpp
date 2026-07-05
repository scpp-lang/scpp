// ch05 §5.10 implementation note (ch06/ch08 Q11): C linkage has no name
// mangling to disambiguate overloads by parameter type, so two
// `extern "C"` declarations sharing a name is an ordinary redefinition
// error, not a legal overload set.
extern "C" {
    int puts(const char* s);
}
extern "C" {
    int puts(int x);
}

int main() {
    return 0;
}
