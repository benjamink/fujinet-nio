# Stage the FatFs sources that ship in the Pico SDK's TinyUSB (lib/tinyusb/lib/fatfs/source)
# next to this directory's ffconf.h, so ff.h picks up *our* configuration. A quoted #include
# looks beside the including file first, and the SDK's own ffconf.h sits beside its ff.h, so
# the sources are copied rather than used in place.
#
#   nio_fatfs_stage(<sdk dir> <output dir>)
# sets NIO_FATFS_SOURCES (ff.c, ffunicode.c, ffsystem.c) and NIO_FATFS_INCLUDE_DIR.
function(nio_fatfs_stage SDK_DIR OUT_DIR)
    set(SRC ${SDK_DIR}/lib/tinyusb/lib/fatfs/source)
    if(NOT EXISTS ${SRC}/ff.c)
        message(FATAL_ERROR "FatFs not found at ${SRC}: the Pico SDK's TinyUSB submodules are not fetched")
    endif()
    file(MAKE_DIRECTORY ${OUT_DIR})
    foreach(f ff.c ff.h ffunicode.c ffsystem.c diskio.h)
        configure_file(${SRC}/${f} ${OUT_DIR}/${f} COPYONLY)
    endforeach()
    configure_file(${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ffconf.h ${OUT_DIR}/ffconf.h COPYONLY)
    set(NIO_FATFS_SOURCES ${OUT_DIR}/ff.c ${OUT_DIR}/ffunicode.c ${OUT_DIR}/ffsystem.c PARENT_SCOPE)
    set(NIO_FATFS_INCLUDE_DIR ${OUT_DIR} PARENT_SCOPE)
endfunction()
