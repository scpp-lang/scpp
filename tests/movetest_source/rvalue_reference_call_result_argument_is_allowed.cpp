// A function's return value is already a fresh rvalue at the call site
// (ch03/ch05 §5.11) -- no std::move needed to pass it into a `T&&`
// parameter.
int make_value() { return 99; }
int take(int&& x) { return x; }
int main() { return take(make_value()) - 99; }
