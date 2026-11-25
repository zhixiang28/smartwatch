# Helix fixed-point MP3 decoder (RealNetworks RPSL/RCSL), vendored from github.com/chmorgan/libhelix-mp3
file(GLOB HELIX_SRCS real/*.c)
idf_component_register(SRCS mp3dec.c mp3tabs.c ${HELIX_SRCS}
                       INCLUDE_DIRS pub
                       PRIV_INCLUDE_DIRS real)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-unused-but-set-variable -Wno-unused-variable -Wno-maybe-uninitialized)
