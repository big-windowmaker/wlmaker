# Copyright (c) 2026 Big WindowMaker contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Translations of wlmaker: The gettext domain "wlmaker".
#
# - Build (target `wlmaker_catalogs`, part of `all`): Compiles each language
#   of LINGUAS, po/<lang>.po, with `msgfmt --check` into
#   <build>/po/locale/<lang>/LC_MESSAGES/wlmaker.mo. Run wlmaker with
#   WLMAKER_LOCALEDIR=<build>/po/locale to use those.
# - Install: ${CMAKE_INSTALL_LOCALEDIR}/<lang>/LC_MESSAGES/wlmaker.mo.
# - `make wlmaker-pot`: Regenerates po/wlmaker.pot from POTFILES.
# - `make wlmaker-update-po`: Merges the template into each po/<lang>.po.
# - Test `i18n_check` (ctest): Catalogs, POTFILES and template up to date.
cmake_minimum_required(VERSION 3.13)

set(WLMAKER_I18N_DOMAIN "wlmaker")

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/LINGUAS" _linguas
  REGEX "^[^#]")
set(_mo_files)
foreach(_lang IN LISTS _linguas)
  string(STRIP "${_lang}" _lang)
  if(_lang STREQUAL "")
    continue()
  endif()
  set(_po "${CMAKE_CURRENT_SOURCE_DIR}/${_lang}.po")
  if(NOT EXISTS "${_po}")
    message(FATAL_ERROR "po/LINGUAS lists ${_lang}, but ${_po} is missing.")
  endif()
  set(_mo_dir "${CMAKE_CURRENT_BINARY_DIR}/locale/${_lang}/LC_MESSAGES")
  set(_mo "${_mo_dir}/${WLMAKER_I18N_DOMAIN}.mo")
  add_custom_command(
    OUTPUT "${_mo}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_mo_dir}"
    COMMAND "${GETTEXT_MSGFMT_EXECUTABLE}" --check -o "${_mo}" "${_po}"
    DEPENDS "${_po}"
    COMMENT "Compiling ${_lang}.po"
    VERBATIM)
  install(
    FILES "${_mo}"
    DESTINATION "${CMAKE_INSTALL_LOCALEDIR}/${_lang}/LC_MESSAGES")
  list(APPEND _mo_files "${_mo}")
endforeach()
add_custom_target(wlmaker_catalogs ALL DEPENDS ${_mo_files})

add_custom_target(
  wlmaker-pot
  COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/update-pot.sh"
    "${PROJECT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/${WLMAKER_I18N_DOMAIN}.pot"
  COMMENT "Generating po/${WLMAKER_I18N_DOMAIN}.pot"
  VERBATIM)

set(_merge_commands)
foreach(_lang IN LISTS _linguas)
  string(STRIP "${_lang}" _lang)
  if(_lang STREQUAL "")
    continue()
  endif()
  list(APPEND _merge_commands
    COMMAND "${GETTEXT_MSGMERGE_EXECUTABLE}" --quiet --update --backup=none
      "${CMAKE_CURRENT_SOURCE_DIR}/${_lang}.po"
      "${CMAKE_CURRENT_SOURCE_DIR}/${WLMAKER_I18N_DOMAIN}.pot")
endforeach()
add_custom_target(
  wlmaker-update-po
  ${_merge_commands}
  COMMENT "Merging po/${WLMAKER_I18N_DOMAIN}.pot into the catalogs"
  VERBATIM)
add_dependencies(wlmaker-update-po wlmaker-pot)

if(BUILD_TESTING)
  add_test(
    NAME i18n_check
    COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/check-i18n.sh"
      "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
endif()
