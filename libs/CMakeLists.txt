# scpp (the language) is designed so that every valid `.scpp` source file
# is also valid, compilable real C++ -- the same principle already
# enforced for src/*.cppm (dual-compiled: real C++ via this bootstrap
# clang++ toolchain, and self-hosting via `scpp build` -- see
# scpp_stdlib_artifacts in the top-level CMakeLists.txt), now extended to
# the stdlib itself. libs/std/**/*.scpp and libs/scpp/**/*.scpp are, until
# now, only ever compiled by the `scpp` compiler (via scpp_stdlib_artifacts's
# `scpp build`); they had never been independently checked against a real
# C++ compiler before this target. This target is that check.
#
# This is compile-only, invariant validation, not a real, used library:
# the resulting object code is never linked into anything (see OBJECT
# below) -- it exists purely to prove/track C++-compilability.
#
# libs/llvm/*.scpp is deliberately NOT included here: unlike every file
# below, it is already plain, ordinary C++ (not scpp-language stdlib
# source at all -- see libs/llvm/CMakeLists.txt's own header comment for
# why), and it is already compiled as real C++ and linked into the real
# `scpp` compiler via the `llvm` target (top-level CMakeLists.txt).
# Re-validating it here would just be duplicate compilation of files that
# already prove their own C++-validity by being part of the real build.
set(SCPP_STDLIB_CXX_VALIDATION_SOURCES
    std/std.scpp
    std/algorithm/std_algorithm.scpp
    std/charconv/std_charconv.scpp
    std/concepts/std_concepts.scpp
    std/cstdlib/std_cstdlib.scpp
    std/expected/std_expected.scpp
    std/format/std_format.scpp
    std/functional/std_functional.scpp
    std/memory/std_memory.scpp
    std/optional/std_optional.scpp
    std/print/std_print.scpp
    std/random/std_random.scpp
    std/string/std_string.scpp
    std/string_view/std_string_view.scpp
    std/thread/std_thread.scpp
    std/unordered_set/std_unordered_set.scpp
    std/vector/std_vector.scpp
    scpp/scpp.scpp
    scpp/enum_cast/scpp_enum_cast.scpp
    scpp/io/scpp_io.scpp
    scpp/rand/scpp_rand.scpp)

# `.scpp` isn't a CMake-recognized extension -- force it back to C++,
# mirroring the exact technique libs/llvm/CMakeLists.txt already uses for
# its own (unrelated, already-real-C++) `.scpp` files.
set_source_files_properties(
    ${SCPP_STDLIB_CXX_VALIDATION_SOURCES}
    PROPERTIES LANGUAGE CXX)

# EXCLUDE_FROM_ALL: this target currently does NOT compile cleanly (see
# below), so it must not be part of the default `ALL` build -- doing so
# would break `cmake --build build` for everyone. It stays available as an
# explicit, opt-in target (`cmake --build build --target
# scpp_stdlib_cxx_validation`) so it can be built on demand to track
# progress as the underlying findings are addressed.
add_library(scpp_stdlib_cxx_validation OBJECT EXCLUDE_FROM_ALL)
target_sources(scpp_stdlib_cxx_validation
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            ${SCPP_STDLIB_CXX_VALIDATION_SOURCES})
target_compile_features(scpp_stdlib_cxx_validation PRIVATE cxx_std_26)

# libs/std/std.scpp declares `export module std;` -- scpp's own,
# self-contained stdlib module, unrelated to (and unaware of) any real C++
# standard library module. It happens to share the literal name "std" with
# the one module name every C++ compiler recognizes specially (module
# names starting with "std" are reserved by the standard,
# [module.reserved]), so real compilers diagnose it. CXX_MODULE_STD must
# also be off for this target specifically: leaving it on would
# additionally try to make the compiler's own real `std` module available
# alongside our own same-named one -- a genuine conflict distinct from
# (and on top of) the reserved-name diagnostic below.
set_target_properties(scpp_stdlib_cxx_validation PROPERTIES CXX_MODULE_STD 0)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Same precedent as scpp_ast's own -Wno-error=unknown-attributes
    # above: scpp vendor attributes ([[scpp::lifetime(...)]],
    # [[scpp::unsafe]], etc.) are valid, ignorable vendor syntax per the
    # standard's attribute rules, but trip Clang's "unknown attribute
    # ignored" warning, which would otherwise be a hard error under this
    # project's global -Werror-equivalent. Likewise for the reserved
    # module name discussed above.
    target_compile_options(scpp_stdlib_cxx_validation PRIVATE
        -Wno-error=unknown-attributes
        -Wno-error=reserved-module-identifier)
endif()

# ---------------------------------------------------------------------
# FINDING (as of this target's introduction): the "every valid scpp file
# is also valid C++" invariant does NOT currently hold for the stdlib.
# `cmake --build build --target scpp_stdlib_cxx_validation` fails with
# real, structural incompatibilities -- not just fixable CMake/warning
# config issues (those are already handled above). Confirmed root causes,
# found by actually attempting this build and reading Clang's own
# diagnostics (see PR description for the full investigation):
#
#  1. Fixed-width/size integer aliases (`size_t`, `uint32_t`, `uint64_t`,
#     ...) are used throughout (e.g. std/vector, std/random, std/string,
#     std/string_view, std/unordered_set, std/functional,
#     scpp/rand) with no declaration anywhere in the visible source --
#     no `#include`, no `import`. scpp itself evidently treats these as
#     compiler-intrinsic/built-in types; real C++ requires
#     <cstddef>/<cstdint> (or `import std;`) to declare them, which these
#     files never do.
#  2. `std::move` is called in multiple files (std/expected, std/memory,
#     std/optional, std/thread, std/vector) but is not defined by any
#     `.scpp` file in the stdlib -- scpp evidently treats it as an
#     intrinsic too, with no real declaration for a C++ compiler to find.
#  3. std/memory/std_memory.scpp's `get() const` accessor places its
#     attribute BEFORE the trailing `const` qualifier
#     (`const T* get() [[scpp::lifetime(this)]] const { ... }`). Standard
#     C++ grammar only allows attribute-specifier-seq AFTER the trailing
#     cv/ref-qualifiers and noexcept-specifier in a function declarator,
#     not before -- so Clang misparses this as two same-signature
#     overloads differing only by return type.
#
# Confirmed directly compiling as real C++ with no changes needed beyond
# the config-level fixes above: std/algorithm, std/charconv,
# std/concepts, std/cstdlib. The remaining files' own status is unknown --
# they transitively `import` one of the files above via a partition
# dependency and so never got compiled in this attempt.
#
# Per the task that introduced this target: these are scpp-language
# stdlib source semantics, not CMake/build-system config -- deciding
# whether/how to change them (e.g. adding real typedefs, defining
# `std::move` for real, reordering the attribute) is a language-design
# call for the project owner, not something to patch unilaterally here.
# ---------------------------------------------------------------------
