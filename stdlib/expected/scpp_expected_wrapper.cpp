#include <cstdlib>

extern "C" {

void scpp_expected_abort() { std::abort(); }

} // extern "C"
