idf_component_register(SRCS "main.c" "gfx.c" "wifi_setup.c" "wifi_ui.c" "ical.c" "calendar.c" "calendar_ui.c" "battery.c" "statusbar.c" "reminders.c" "remote_log.c" "ota.c" "spotify.c" "spotify_ui.c" "library_ui.c" "pet_ui.c" "sprites_ui.c" "agent.c" "font_ui.c" "font_clock.c" "font_mono.c" "icons_ui.c" "board_square.c" "audio.c" "chat.c" "chat_ui.c" INCLUDE_DIRS ".")
# A release build (for a board that leaves our hands) takes only a paired Mac's key, not the development token:
#   idf.py -B build-release -D BOBBLE_RELEASE=1 build    (its own build folder, so everyday builds stay development ones)
if(BOBBLE_RELEASE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOBBLE_RELEASE=1)
endif()
# Which build this is (yymmddHHMM, set by agent_bridge/build_app.sh --release): the board reports it in GET /info so the
# Mac app knows when a release has newer firmware. Everyday builds leave it at 0.
if(BOBBLE_BUILD)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOBBLE_BUILD=${BOBBLE_BUILD})
endif()
# The 4" square board (Waveshare ESP32-S3-Touch-LCD-4B) is its own build, with its own folder and settings:
#   idf.py -B build-square -D BOBBLE_SQUARE=1 -D SDKCONFIG=build-square/sdkconfig \
#          -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.square" build
if(BOBBLE_SQUARE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_SQUARE=1)
endif()
