# libsmb2 v6.0.0 (https://github.com/sahlberg/libsmb2, LGPL-2.1; see COPYING),
# for the Files app's SMB shares; two local fixes in lib/ (see README.md).
file(GLOB smb2_sources "lib/*.c")
list(FILTER smb2_sources EXCLUDE REGEX "krb5-wrapper\\.c$")
idf_component_register(
    SRCS ${smb2_sources}
    INCLUDE_DIRS "include" "include/smb2"
    PRIV_INCLUDE_DIRS "lib" "include/esp"
    PRIV_REQUIRES lwip)
# This IDF has no readv/writev/getlogin_r: use libsmb2's own (compat.c).
target_compile_definitions(${COMPONENT_LIB} PRIVATE HAVE_CONFIG_H "_U_=__attribute__((unused))" NEED_READV NEED_WRITEV
                           NEED_GETLOGIN_R)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-variable -Wno-unused-function -Wno-maybe-uninitialized
                       -Wno-format -Wno-char-subscripts -Wno-implicit-fallthrough -Wno-unused-but-set-variable
                       -Wno-incompatible-pointer-types -Wno-discarded-qualifiers -Wno-stringop-truncation
                       -Wno-error -Wno-type-limits -Wno-sign-compare)
