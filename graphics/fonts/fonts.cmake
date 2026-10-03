# The guest's fonts (ADR-0043 test 2, decision 5), which runtime/bionic/CMakeLists.txt includes, so that <build>/system
# is put together in one place: Roboto, DroidSansMono and DroidSansFallbackFull in /system/fonts, and the font lists
# of Typeface and SystemFonts, /system/etc/fonts.xml and font_fallback.xml of this folder, cut to them. Copied at
# build time, each again when its source changes. The fonts are in the sources list's graphics set, so bionic's
# configure step stops without them rather than build a /system whose text aborts in native code.
set(guest_font_sources roboto_fonts/Roboto-Regular.ttf roboto_fonts/RobotoStatic-Regular.ttf
  frameworks_base_hwui/data/fonts/DroidSansMono.ttf frameworks_base_hwui/data/fonts/DroidSansFallbackFull.ttf)
set(guest_fonts)
foreach(font IN LISTS guest_font_sources)
  if(NOT EXISTS "${APK_SOURCES}/${font}")
    message(FATAL_ERROR "${APK_SOURCES}/${font} is missing: run tools/fetch-sources.py --set graphics")
  endif()
  get_filename_component(name "${font}" NAME)
  add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/system/fonts/${name}"
    COMMAND ${CMAKE_COMMAND} -E copy "${APK_SOURCES}/${font}" "${CMAKE_BINARY_DIR}/system/fonts/${name}"
    DEPENDS "${APK_SOURCES}/${font}" VERBATIM)
  list(APPEND guest_fonts "${CMAKE_BINARY_DIR}/system/fonts/${name}")
endforeach()
foreach(font_list IN ITEMS fonts.xml font_fallback.xml)
  add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/system/etc/${font_list}"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_LIST_DIR}/${font_list}"
      "${CMAKE_BINARY_DIR}/system/etc/${font_list}"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/${font_list}" VERBATIM)
  list(APPEND guest_fonts "${CMAKE_BINARY_DIR}/system/etc/${font_list}")
endforeach()
add_custom_target(guest_fonts ALL DEPENDS ${guest_fonts})
