// ch05 §5.1/§5.12: an immediately-invoked lambda's own literal
// (`[capture](args){...}(...)`) is stored in the enclosing Call
// expression's `expr.lhs`, never visited at all before check_call_arguments
// also started visiting a method call's own receiver through the same
// field (see method_call_on_moved_out_object_is_rejected.cpp) -- a real,
// discovered-and-fixed gap: an IIFE's captures went completely unchecked,
// so this init-capture of an already-moved-out std::unique_ptr used to be
// silently accepted instead of rejected the same way capturing it in a
// stored (non-IIFE) closure already correctly is (see
// closure_by_value_capture_of_unique_ptr_is_rejected.cpp).
int main() {
    std::unique_ptr<int> p = std::make_unique<int>(5);
    std::unique_ptr<int> q = std::move(p);
    return [p = std::move(p)]() { return 0; }();
}
