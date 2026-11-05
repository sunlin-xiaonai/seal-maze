idf_component_register(SRCS "main.c" "maze.c" "game.c" "ring.c" "view.c" "web.c" "sound.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "web/index.html"
    REQUIRES yaykit_board lvgl__lvgl yaykit_app yaykit_key yaykit_link yaykit_ui yaykit_motion yaykit_speak yaykit_wifi
             yaykit_cloud
    PRIV_REQUIRES nvs_flash console esp_timer esp_http_server esp_netif esp_wifi espressif__mdns)
# The shared font partition; the app's own full install writes it too. Found through yaykit_board, wherever the
# shared components are (this repository or the development kit).
idf_component_get_property(board_dir yaykit_board COMPONENT_DIR)
esptool_py_flash_to_partition(flash "fonts" "${board_dir}/fonts/cjk/yaykit_fonts.bin")
target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_VERSION="${PROJECT_VER}")
