// ch05 §5.10: "By-value vs. by-reference is a separate, orthogonal axis
// ... a bare lvalue argument is only ever viable against a T&/const T&
// parameter, never a T one; conversely std::move(x) (or an ordinary
// prvalue/temporary) is only viable against a T parameter." A literal
// (prvalue) can only match f(int); a named variable (lvalue) can only
// match f(int&).
safe int f(int x) {
    return x * 2;
}

safe int f(int& x) {
    x = x + 1;
    return x * 3;
}

int main() {
    int y = 10;
    int r1 = f(5); // prvalue -> f(int): 5*2 = 10
    int r2 = f(y); // lvalue -> f(int&): y becomes 11, returns 11*3 = 33
    return r1 + r2 + y; // 10 + 33 + 11 = 54
}
