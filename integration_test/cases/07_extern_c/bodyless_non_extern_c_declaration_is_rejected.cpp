// ch02 §2.1: "a function declaration without a body is only supported
// for extern "C" ...; every other function must have a definition." A
// bodyless, non-extern-C declaration (e.g. attempting a C++-style forward
// declaration for later use) is rejected.
int foo(int x);

int main() {
    return 0;
}
