# Binary file -> C array (any C compiler; #embed needs GCC 15 / Clang 19):
# cmake -DIN=<file> -DOUT=<header> -DNAME=<symbol> -P embed_bin.cmake
file(READ "${IN}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," arr "${hex}")
file(WRITE "${OUT}" "static const unsigned char ${NAME}[] = {${arr}};\n")
