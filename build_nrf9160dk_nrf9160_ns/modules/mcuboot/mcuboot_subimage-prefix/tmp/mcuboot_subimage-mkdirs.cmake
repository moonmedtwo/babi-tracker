# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/workdir/bootloader/mcuboot/boot/zephyr"
  "/workdir/project/build_nrf9160dk_nrf9160_ns/mcuboot"
  "/workdir/project/build_nrf9160dk_nrf9160_ns/modules/mcuboot/mcuboot_subimage-prefix"
  "/workdir/project/build_nrf9160dk_nrf9160_ns/modules/mcuboot/mcuboot_subimage-prefix/tmp"
  "/workdir/project/build_nrf9160dk_nrf9160_ns/modules/mcuboot/mcuboot_subimage-prefix/src/mcuboot_subimage-stamp"
  "/workdir/project/build_nrf9160dk_nrf9160_ns/modules/mcuboot/mcuboot_subimage-prefix/src"
  "/workdir/project/build_nrf9160dk_nrf9160_ns/modules/mcuboot/mcuboot_subimage-prefix/src/mcuboot_subimage-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/workdir/project/build_nrf9160dk_nrf9160_ns/modules/mcuboot/mcuboot_subimage-prefix/src/mcuboot_subimage-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/workdir/project/build_nrf9160dk_nrf9160_ns/modules/mcuboot/mcuboot_subimage-prefix/src/mcuboot_subimage-stamp${cfgdir}") # cfgdir has leading slash
endif()
