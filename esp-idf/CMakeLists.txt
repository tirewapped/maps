file(GLOB MAPS_SRCS "src/*.cpp")

idf_component_register(
    SRCS ${MAPS_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES
        spangap-core
        spangap-lcd
    PRIV_REQUIRES
        espressif__esp_jpeg
)
