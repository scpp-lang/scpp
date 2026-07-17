add_library(scpp_codegen)
target_sources(scpp_codegen
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            codegen.cppm
            errors.cppm
            api.cppm
            orchestration.cppm
            debug.cppm
            semantics.cppm
            layout.cppm
            object_model.cppm
            functions.cppm
            expressions.cppm
            lifetime.cppm
            statements.cppm)
target_include_directories(scpp_codegen SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})
target_compile_definitions(scpp_codegen PUBLIC ${LLVM_DEFINITIONS_LIST})
target_link_libraries(scpp_codegen PUBLIC scpp_ast scpp_constexpr ${LLVM_LIBS})
target_compile_features(scpp_codegen PRIVATE cxx_std_23)
