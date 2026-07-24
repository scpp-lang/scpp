#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

extern "C" {

void* scpp_vector_allocate(std::uint64_t bytes, std::uint64_t align) {
    return ::operator new(static_cast<std::size_t>(bytes),
                          std::align_val_t(static_cast<std::size_t>(align)));
}

void scpp_vector_deallocate(void* ptr, std::uint64_t align) {
    if (ptr == nullptr) return;
    ::operator delete(ptr, std::align_val_t(static_cast<std::size_t>(align)));
}

void scpp_vector_relocate_bytes(void* dst, void* src, std::uint64_t bytes) {
    if (bytes == 0) return;
    const std::size_t size = static_cast<std::size_t>(bytes);
    std::memmove(dst, src, size);
    std::memset(src, 0, size);
}

}
