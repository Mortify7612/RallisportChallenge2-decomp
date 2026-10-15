# Text file -> C array, for shader source the runtime compiles itself:
# cmake -DIN=<file> -DOUT=<header> -DNAME=<symbol> -P embed.cmake
file(READ "${IN}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," arr "${hex}")
file(WRITE "${OUT}" "static const char ${NAME}[] = {${arr}0};\n")
