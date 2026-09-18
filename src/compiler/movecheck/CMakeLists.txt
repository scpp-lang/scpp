add_library(scpp_movecheck)
target_sources(scpp_movecheck
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            movecheck.cppm
            errors.cppm
            state.cppm
            signatures.cppm
            types.cppm
            calls.cppm
            borrows.cppm
            dataflow.cppm
            interfaces.cppm
            threadsafety.cppm
            generics_support.cppm
            lambdas.cppm
            monomorphize.cppm)
target_link_libraries(scpp_movecheck PUBLIC scpp_ast scpp_mir scpp_constexpr)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(scpp_movecheck PRIVATE -Wno-unknown-attributes)
endif()
