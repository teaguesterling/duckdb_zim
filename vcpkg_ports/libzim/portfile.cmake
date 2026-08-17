vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO openzim/libzim
    REF "${VERSION}"
    SHA512 3f635eaf4363bf12b92f12a4d089883f10bb8cfe4c572876ab410d88273f8a44455ce93ef74bcb0836051ea3dd51d39f25f703d6c01d6599cce0a8801bc09fba
    HEAD_REF main
    PATCHES
        cross-builds.diff
        dllexport.diff
        subdirs.diff
        # Local overlay only: adds the public zim::IRandomAccessReader API so a
        # ZIM can be opened from a user-provided positioned-read source (used for
        # remote HTTP/S3 archives). Proposed upstream; drop this overlay once a
        # libzim release carries it. See README.md.
        stream-reader-api.patch
        # Local overlay only: libzim writes "No stemming for language 'X'" to
        # std::cout when Xapian has no stemmer for the archive's language (e.g. zh),
        # polluting the caller's stdout on every search/suggest open. Drop the print
        # (the catch still leaves stemming disabled, which is correct). Propose
        # upstream; drop this overlay once libzim stops printing to stdout.
        no-stemming-stdout.patch
        # Local overlay only: libzim's writer defines INFO(e) as an UNGATED
        # `std::cout << e`, unlike TINFO/TPROGRESS which check m_verbose. Four of its
        # six call sites fire on a normal write ("Set entry indices", "Index titles",
        # "Detect dangling redirects", "Detect loops and/or blind chains of
        # redirects"); the other two announce removed invalid redirections. That
        # pollutes the caller's stdout on every COPY TO. Keep log_info(), drop the
        # print. Propose upstream; drop this overlay once libzim stops printing.
        no-writer-stdout.patch
)

set(EXTRA_OPTIONS "")

if(NOT "xapian" IN_LIST FEATURES)
    list(APPEND EXTRA_OPTIONS "-Dwith_xapian=false")
endif()

vcpkg_configure_meson(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
      -Dexamples=false
      ${EXTRA_OPTIONS}
)

vcpkg_install_meson()

vcpkg_copy_pdbs()

vcpkg_fixup_pkgconfig()

if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/zim/zim.h" "defined(LIBZIM_IMPORT_DLL)" "1")
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
