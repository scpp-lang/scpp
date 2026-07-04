// ch06 scalar table: "if/while conditions must already be bool" -- no
// implicit int-to-bool conversion in a condition, unlike real C++.
int main() {
    if (5) {
        return 1;
    }
    return 0;
}
