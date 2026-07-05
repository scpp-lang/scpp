// Counterpart for `||`: since the left operand is `true`, `||` must not
// evaluate `side_effect` (which would abort via division by zero).
bool side_effect() {
    int x = 10;
    int y = 0;
    int z = x / y;
    return z > 0;
}

int main() {
    bool a = true;
    if (a || side_effect()) {
        return 1;
    }
    return 0;
}
