// Prerequisite fix for move construction: a constructor-call VarDecl's
// own arguments are now visible to the dataflow checker (previously a
// `has_ctor_args` VarDecl lowered to an argument-blind MIR Declare, so
// `std::move(p)` here never marked `p` moved-out at all). `p` is
// moved-out by being passed to Holder's constructor, so reading it
// afterward is ill-formed.
class Holder {
public:
    Holder(std::unique_ptr<int> p) { return; }
};
int main() {
    std::unique_ptr<int> p = std::make_unique<int>(5);
    Holder h(std::move(p));
    return *p;
}
