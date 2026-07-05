// ch01 §1.3: "scpp's unsafe { } is an ordinary block: it behaves exactly
// like a plain { } compound statement -- locals declared inside go out of
// scope at the closing }." Reading `local_var` after the block closes
// must be rejected, exactly as for a plain `{ }` scope.
safe int f() {
    unsafe {
        int local_var = 5;
    }
    return local_var;
}

int main() {
    return f();
}
