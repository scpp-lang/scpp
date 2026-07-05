// Sanity check for the two short-circuit tests above: when the left
// operand of `&&` is `true`, the right operand *must* actually be
// evaluated -- proving those tests pass because of real short-circuit
// evaluation, not because `side_effect` is simply never called at all.
bool side_effect() {
    int x = 10;
    int y = 0;
    int z = x / y;
    return z > 0;
}

int main() {
    bool a = true;
    if (a && side_effect()) {
        return 1;
    }
    return 0;
}
