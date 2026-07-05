// ch06 scalar table: "No implicit conversion to or from any other type"
// for bool -- the reverse direction (bool -> int) must be rejected too,
// symmetrically with int -> bool.
int main() {
    int x = true;
    return x;
}
