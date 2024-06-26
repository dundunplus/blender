# SPDX-FileCopyrightText: 2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later


set(DEFLATE_EXTRA_ARGS
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  -DLIBDEFLATE_BUILD_STATIC_LIB=ON
  -DLIBDEFLATE_BUILD_SHARED_LIB=OFF
)

ExternalProject_Add(external_deflate
  URL file://${PACKAGE_DIR}/${DEFLATE_FILE}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  URL_HASH ${DEFLATE_HASH_TYPE}=${DEFLATE_HASH}
  PREFIX ${BUILD_DIR}/deflate
  CMAKE_GENERATOR ${PLATFORM_ALT_GENERATOR}

  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=${LIBDIR}/deflate
    ${DEFAULT_CMAKE_FLAGS}
    ${DEFLATE_EXTRA_ARGS}

  INSTALL_DIR ${LIBDIR}/deflate
)
