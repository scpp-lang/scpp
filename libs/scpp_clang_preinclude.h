#ifndef __SCPP_NON_MODULE__
module;
#endif
#include <cstddef>
#include <cstdint>
#include <new>

// Standard type aliases in global namespace for SCPP intrinsics
using size_t = std::size_t;
using ptrdiff_t = std::ptrdiff_t;
using nullptr_t = std::nullptr_t;

using int8_t = std::int8_t;
using int16_t = std::int16_t;
using int32_t = std::int32_t;
using int64_t = std::int64_t;

using uint8_t = std::uint8_t;
using uint16_t = std::uint16_t;
using uint32_t = std::uint32_t;
using uint64_t = std::uint64_t;

using float32_t = float;
using float64_t = double;

// Standard std::move and std::forward intrinsics
namespace std {
template <typename T>
struct remove_reference { using type = T; };

template <typename T>
struct remove_reference<T&> { using type = T; };

template <typename T>
struct remove_reference<T&&> { using type = T; };

template <typename T>
using remove_reference_t = typename remove_reference<T>::type;

template <typename T>
constexpr remove_reference_t<T>&& move(T&& t) noexcept {
    return static_cast<remove_reference_t<T>&&>(t);
}

template <typename T>
constexpr T&& forward(remove_reference_t<T>& t) noexcept {
    return static_cast<T&&>(t);
}

template <typename T>
constexpr T&& forward(remove_reference_t<T>&& t) noexcept {
    return static_cast<T&&>(t);
}
}
