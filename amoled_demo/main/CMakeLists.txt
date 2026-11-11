idf_component_register(SRCS "main.c" "gfx.c" "wifi_setup.c" "wifi_ui.c" "ical.c" "calendar.c" "calendar_ui.c" "battery.c" "statusbar.c" "reminders.c" "remote_log.c" "ota.c" "spotify.c" "spotify_ui.c" "library_ui.c" "pet_ui.c" "sprites_ui.c" "agent.c" "font_ui.c" "font_clock.c" "font_mono.c" "icons_ui.c" "board_square.c" "audio.c" "chat.c" "chat_ui.c" "boot_ui.c" "stb_jpeg.c" INCLUDE_DIRS ".")
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
# Restarting the panel's DMA every frame shifts and breaks the picture (see sdkconfig.defaults): never build with it
if(CONFIG_LCD_RGB_RESTART_IN_VSYNC)
    message(FATAL_ERROR "CONFIG_LCD_RGB_RESTART_IN_VSYNC is on in ${SDKCONFIG}: delete that file and build again")
endif()
