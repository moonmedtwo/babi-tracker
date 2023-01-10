# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/workdir/modules/tee/tf-m/trusted-firmware-m"
  "/workdir/project/build_circuitdojo_feather_nrf9160_ns/tfm"
  "/workdir/project/build_circuitdojo_feather_nrf9160_ns/modules/trusted-firmware-m/tfm-prefix"
  "/workdir/project/build_circuitdojo_feather_nrf9160_ns/modules/trusted-firmware-m/tfm-prefix/tmp"
  "/workdir/project/build_circuitdojo_feather_nrf9160_ns/modules/trusted-firmware-m/tfm-prefix/src/tfm-stamp"
  "/workdir/project/build_circuitdojo_feather_nrf9160_ns/modules/trusted-firmware-m/tfm-prefix/src"
  "/workdir/project/build_circuitdojo_feather_nrf9160_ns/modules/trusted-firmware-m/tfm-prefix/src/tfm-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/workdir/project/build_circuitdojo_feather_nrf9160_ns/modules/trusted-firmware-m/tfm-prefix/src/tfm-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/workdir/project/build_circuitdojo_feather_nrf9160_ns/modules/trusted-firmware-m/tfm-prefix/src/tfm-stamp${cfgdir}") # cfgdir has leading slash
endif()
