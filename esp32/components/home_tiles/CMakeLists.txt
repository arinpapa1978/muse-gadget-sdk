# 우리 집 정보 타일. CONFIG_HOME_TILES 가 꺼져 있으면 아무것도 넣지 않음
set(srcs)
if(CONFIG_HOME_TILES)
    set(srcs "home_tiles.c" "home_tts.c" "fonts/home_kr_16.c" "fonts/home_kr_20.c" "fonts/home_num_26.c" "fonts/home_num_80.c" "fonts/home_num_88l.c")
endif()
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_http_client esp-tls mbedtls nvs_flash esp_timer
)
# 화면 글자는 길면 잘려도 괜찮음 (잘림 경고를 오류로 보지 않음)
if(CONFIG_HOME_TILES)
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation -Wno-error)
endif()
