// ch02: "safe calls unsafe | Must be wrapped in unsafe { }, otherwise a
// compile error."
void raw() {
    return;
}

safe void caller() {
    raw();
    return;
}

int main() {
    caller();
    return 0;
}
