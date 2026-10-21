idf_component_register(SRCS "main.c" "gfx.c" "wifi_setup.c" "wifi_ui.c" "ical.c" "calendar.c" "calendar_ui.c" "battery.c" "statusbar.c" "reminders.c" "remote_log.c" "ota.c" "spotify.c" "spotify_ui.c" "library_ui.c" "pet_ui.c" "agent.c" "font_ui.c" "icons_ui.c" INCLUDE_DIRS ".")
# A release build (for a board that leaves our hands) takes only a paired Mac's key, not the development token:
#   idf.py -B build-release -D BOBBLE_RELEASE=1 build    (its own build folder, so everyday builds stay development ones)
if(BOBBLE_RELEASE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOBBLE_RELEASE=1)
endif()
