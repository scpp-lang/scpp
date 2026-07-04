// ch06: ordinary short-circuit `&&` semantics. `side_effect` divides by
// zero (an unconditional abort() per ch05 §5.8), so if `&&`'s right
// operand were (wrongly) evaluated even when the left operand is
// `false`, the process would abort (exit 134) instead of returning
// cleanly.
bool side_effect() {
    int x = 10;
    int y = 0;
    int z = x / y;
    return z > 0;
}

int main() {
    bool a = false;
    if (a && side_effect()) {
        return 2;
    }
    return 0;
}
