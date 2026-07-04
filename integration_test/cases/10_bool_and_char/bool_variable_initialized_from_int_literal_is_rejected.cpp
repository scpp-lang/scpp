// ch06 scalar table: "No implicit conversion to or from any other type"
// for bool -- unlike real C++ (where `int` implicitly converts to
// `bool`), scpp requires an explicit cast. Initializing a `bool` local
// directly from an `int` literal should therefore be rejected.
int main() {
    bool b = 5;
    return 0;
}
