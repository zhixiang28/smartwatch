# Helix fixed-point AAC-LC / HE-AAC (SBR) decoder (RealNetworks RPSL/RCSL),
# vendored from github.com/pschatzmann/arduino-libhelix (src/libhelix-aac) with small shims.
file(GLOB AAC_SRCS src/*.c)
idf_component_register(SRCS ${AAC_SRCS}
                       INCLUDE_DIRS include
                       PRIV_INCLUDE_DIRS src
                       REQUIRES heap)
target_compile_definitions(${COMPONENT_LIB} PRIVATE HELIX_FEATURE_AUDIO_CODEC_AAC_SBR=1 USE_DEFAULT_STDLIB=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -w)
