#include <iostream>
#include <string>

extern "C" void* scpp_string_new(const char* s);

extern "C" {

void* scpp_io_read_line(int* status) {
    if (status != nullptr) *status = 2;
    try {
        std::string line;
        if (std::getline(std::cin, line)) {
            if (status != nullptr) *status = 0;
            return scpp_string_new(line.c_str());
        }
        if (status != nullptr) *status = std::cin.eof() ? 1 : 2;
        return nullptr;
    } catch (...) {
        if (status != nullptr) *status = 2;
        return nullptr;
    }
}

} // extern "C"
