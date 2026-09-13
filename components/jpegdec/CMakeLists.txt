# Local override of the bitbank2/jpegdec registry component (v1.6.2).
# Sources are verbatim upstream (see LICENSE); only the build flags below
# differ: upstream trips IDF v6 -Werror on two benign warnings
# (a 32-bit pointer-alignment check and a long-standing shift idiom),
# so they are downgraded to warnings for this component only.
idf_component_register(SRCS "JPEGDEC.c" "s3_simd_420.S" "s3_simd_444.S"
                       "jpeg_class.cpp"
                       INCLUDE_DIRS "include")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=pointer-to-int-cast
    -Wno-error=shift-negative-value)
