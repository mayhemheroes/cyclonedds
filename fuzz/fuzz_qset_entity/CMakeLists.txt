#
# Copyright(c) 2021 ZettaScale Technology and others
#
# This program and the accompanying materials are made available under the
# terms of the Eclipse Public License v. 2.0 which is available at
# http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
# v. 1.0 which is available at
# http://www.eclipse.org/org/documents/edl-v10.php.
#
# SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
#

project(fuzz_qset_entity LANGUAGES C)
cmake_minimum_required(VERSION 3.5)

if(NOT TARGET CycloneDDS::ddsc)
  # Find the CycloneDDS package.
  find_package(CycloneDDS REQUIRED)
endif()

idlc_generate(TARGET DataRepresentationTypes FILES DataRepresentationTypes.idl WARNINGS no-implicit-extensibility)

add_executable(fuzz_qset_entity fuzz_qset_entity.c)
target_include_directories(
  fuzz_qset_entity PRIVATE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../src/core/ddsc/tests>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../src/core/ddsi/src>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../src/core/ddsi/include>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../src/ddsrt/include>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../src/ddsrt/src>")
target_link_libraries(fuzz_qset_entity DataRepresentationTypes CycloneDDS::ddsc $ENV{LIB_FUZZING_ENGINE})
