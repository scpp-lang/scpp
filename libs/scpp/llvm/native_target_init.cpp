// A small, deliberate exception to this package's "bind straight to
// official LLVM-C, no custom wrapper of any kind" rule (see
// scpp_llvm.scpp's own top-of-file comment) -- and the *only* one
// in this package. Real LLVM-C's LLVMInitializeNativeTarget()/
// LLVMInitializeNativeAsmPrinter() (llvm-c/Target.h) are `static inline`
// functions defined entirely *in that header*, expanding (via the
// LLVM_NATIVE_TARGET/LLVM_NATIVE_ASMPRINTER macros, themselves set by
// LLVM's own build configuration) to whichever concrete backend the
// host was actually built for -- e.g. LLVMInitializeX86Target() on this
// machine. Unlike every other function this package wraps, there is
// therefore no real, ABI-stable, exported `LLVMInitializeNativeTarget`
// symbol in libLLVM's own shared library to declare `extern "C"` and
// link against directly -- confirmed empirically (`nm -D
// --defined-only libLLVM-22.so | grep LLVMInitializeNative` finds
// nothing, while the concrete-architecture symbols it expands to, e.g.
// `LLVMInitializeX86Target`, are real, exported symbols). Hard-coding
// scpp_llvm.scpp's own extern "C" declarations to one specific
// architecture's concrete symbol names (the only alternative that needs
// no shim at all) would silently break this package -- and the
// compiler's own driver.cppm, the one caller that needs "native, whatever
// this host is" initialization rather than a specific architecture -- on
// any host LLVM wasn't built natively for x86, so that option was
// rejected.
//
// This file is deliberately plain, ordinary C++ (a `.cpp`, not a
// `.scpp`) -- unlike every other source file in this directory, it is
// never `import`ed by anything (scpp or real C++ alike), only compiled
// to object code and linked in. That happens via two entirely separate
// paths, mirroring scpp_llvm.scpp's own dual-compilation split: (a)
// libs/scpp/scpp.toml's `additional_objs` mechanism folds it into
// libscpp.scppa for real scpp users (see io/scpp_io_wrapper.cpp for the
// pre-existing sibling entry this mirrors), and (b) the root
// CMakeLists.txt's small scpp_llvm_native_target_init static-library
// target links it into the compiler's own independent build. Either way
// it never needs to be valid scpp -- which is exactly what lets it
// `#include <llvm-c/Target.h>` and reach the two real inline functions
// at all (scpp_llvm.scpp itself cannot: it has no preprocessor
// once compiled as scpp). It re-exports their result under new,
// distinct `extern "C"` names (scpp_llvm_* below) that
// scpp_llvm.scpp's own extern "C" block declares and Target's
// initialize_native_handle/initialize_native_asm_printer_handle call
// through -- the smallest possible shim for this one confirmed gap.
#include <llvm-c/Target.h>

extern "C" {

int scpp_llvm_initialize_native_target() { return LLVMInitializeNativeTarget(); }

int scpp_llvm_initialize_native_asm_printer() { return LLVMInitializeNativeAsmPrinter(); }

} // extern "C"
