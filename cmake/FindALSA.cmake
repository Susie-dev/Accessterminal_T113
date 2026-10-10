# FindALSA 的补充模块：CMake 自 3.x 起自带 FindALSA，但为兼容部分
# 精简工具链（不包含该模块）此处提供最小替代实现。
# 若 CMake 已提供 FindALSA，则本文件不会被使用。
#
# 定义：ALSA_FOUND / ALSA_INCLUDE_DIRS / ALSA_LIBRARIES / ALSA::ALSA

if(TARGET ALSA::ALSA)
    return()
endif()

find_path(ALSA_INCLUDE_DIR NAMES alsa/asoundlib.h)
find_library(ALSA_LIBRARY NAMES asound libasound.so libasound.a)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ALSA
    REQUIRED_VARS ALSA_LIBRARY ALSA_INCLUDE_DIR)

if(ALSA_FOUND)
    set(ALSA_INCLUDE_DIRS ${ALSA_INCLUDE_DIR})
    set(ALSA_LIBRARIES ${ALSA_LIBRARY})
    if(NOT TARGET ALSA::ALSA)
        add_library(ALSA::ALSA UNKNOWN IMPORTED)
        set_target_properties(ALSA::ALSA PROPERTIES
            IMPORTED_LOCATION "${ALSA_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${ALSA_INCLUDE_DIR}")
    endif()
endif()

mark_as_advanced(ALSA_INCLUDE_DIR ALSA_LIBRARY)
