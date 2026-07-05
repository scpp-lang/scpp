// ch02 boundary table: "unsafe calls safe | Freely allowed. A safe
// function is safe for any caller." `main` is an ordinary native
// function calling a `safe` one directly, no `unsafe { }` needed.
safe int double_it(int x) {
    return x * 2;
}

int main() {
    return double_it(21);
}
