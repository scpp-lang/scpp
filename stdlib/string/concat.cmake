# concat.cmake
#
# Writes SRC1's content immediately followed by SRC2's content to OUT.
# Invoked as `cmake -DSRC1=... -DSRC2=... -DOUT=... -P concat.cmake` (see
# CMakeLists.txt) -- plain CMake file(READ)/file(WRITE), no reliance on a
# shell's `cat`. Needed because scpp v0.1 has no multi-file/include
# mechanism yet (a single `scpp build` invocation compiles exactly one
# input file), so String.cpp (the class) and demo.cpp (a consumer) must be
# concatenated into one file before being built.
file(READ "${SRC1}" content1)
file(READ "${SRC2}" content2)
file(WRITE "${OUT}" "${content1}${content2}")
