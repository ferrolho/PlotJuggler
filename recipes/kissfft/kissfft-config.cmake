# Platform-neutral kissfft package config for the PJ4 pixi build (static,
# datatype=float — the recipe's only variant). Replaces upstream's installed
# config, which (a) declares cmake_minimum_required(VERSION 3.3), rejected
# outright by CMake >= 4 (the conda-forge cmake), and (b) only defines the
# canonical kissfft::kissfft target when a datatype COMPONENT is requested.
# find_library resolves kissfft-float.lib (windows) or libkissfft-float.a
# (unix) alike. Same committed-config pattern as luau/cloudini/libmcap.

if(TARGET kissfft::kissfft)
  return()
endif()

get_filename_component(_kissfft_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

find_library(_kissfft_lib
  NAMES kissfft-float libkissfft-float
  HINTS "${_kissfft_prefix}/lib"
  NO_DEFAULT_PATH REQUIRED)

add_library(kissfft::kissfft STATIC IMPORTED)
set_target_properties(kissfft::kissfft PROPERTIES
  IMPORTED_LOCATION "${_kissfft_lib}"
  # Headers install to include/kissfft; consumers do #include <kiss_fftr.h>,
  # matching how the Conan package resolves them.
  INTERFACE_INCLUDE_DIRECTORIES "${_kissfft_prefix}/include/kissfft"
  # kiss_fft.h already defaults kiss_fft_scalar to float, but the explicit
  # define mirrors the upstream target exactly.
  INTERFACE_COMPILE_DEFINITIONS "kiss_fft_scalar=float")

# Datatype-suffixed companion, for name parity with upstream's config.
add_library(kissfft::kissfft-float INTERFACE IMPORTED)
set_target_properties(kissfft::kissfft-float PROPERTIES
  INTERFACE_LINK_LIBRARIES kissfft::kissfft)

set(kissfft_FOUND TRUE)
