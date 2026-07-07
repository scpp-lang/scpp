import std;
// Same prerequisite fix (see constructor_argument_move_marks_source_moved_out.cpp):
// constructor arguments are now genuinely checked, not just tracked for
// move-state -- a bare (non-move) std::unique_ptr lvalue is never a
// legitimate by-value constructor argument, exactly like an ordinary
// function call (ch05 §5.1).
class Holder {
public:
    Holder(std::unique_ptr<int> p) { return; }
};
int main() {
    std::unique_ptr<int> p = std::make_unique<int>(5);
    Holder h(p);
    return 0;
}
