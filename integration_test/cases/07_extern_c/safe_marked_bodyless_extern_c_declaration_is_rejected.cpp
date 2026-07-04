// ch02 §2.1: "writing safe extern "C" int foo(int x); is a compile error
// (cannot mark an external declaration safe: its implementation isn't
// visible to the compiler)."
safe extern "C" int foo(int x);

int main() {
    return 0;
}
