// Calling a function that is defined further down the file than its call
// site must work without a forward declaration -- scpp registers every
// function's signature before lowering any body (ch07's "name resolution"
// stage runs whole-program before codegen).
int main() {
    return helper();
}

int helper() {
    return 3;
}
