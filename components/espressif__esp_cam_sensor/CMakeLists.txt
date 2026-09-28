set(srcs "src/esp_cam_sensor.c" "src/esp_cam_sensor_xclk.c" "src/esp_cam_motor.c")

# SPI camera path (esp_cam_ctlr_spi_cam + bundled spi_slave) is incompatible with ESP-IDF 6.0+:
#   - esp_private/spi_slave_internal.h removed / relocated
#   - bundled spi_slave.c conflicts with IDF driver
# OV02C10 on JC1060P470C uses MIPI-CSI only — skip SPI sources entirely.
# (On IDF 5.5 with SPI sensors you would need cam_sensor >= 2.0.1 instead.)
if(IDF_VERSION_MAJOR LESS 6)
    list(APPEND srcs "src/driver_cam/esp_cam_ctlr_spi_cam.c")
    if(CONFIG_SPIRAM)
        list(APPEND srcs "src/driver_spi/spi_slave.c")
    endif()
endif()

set(include_dirs "include")
set(requires "driver" "esp_sccb_intf" "esp_driver_cam")
set(priv_requires "esp_driver_gpio" "esp_timer" "esp_pm")

if(CONFIG_CAMERA_XCLK_USE_LEDC)
    list(APPEND requires "esp_driver_ledc")
endif()

if(CONFIG_CAMERA_OV02C10)
    list(APPEND srcs "sensors/ov02c10/ov02c10.c")
    list(APPEND include_dirs "sensors/ov02c10/include")
    list(APPEND priv_include_dirs "sensors/ov02c10/private_include")
endif()

if(CONFIG_CAMERA_OV2640)
    list(APPEND srcs "sensors/ov2640/ov2640.c")
    list(APPEND include_dirs "sensors/ov2640/include")
    list(APPEND priv_include_dirs "sensors/ov2640/private_include")
endif()

if(CONFIG_CAMERA_OV5640)
    list(APPEND srcs "sensors/ov5640/ov5640.c")
    list(APPEND include_dirs "sensors/ov5640/include")
    list(APPEND priv_include_dirs "sensors/ov5640/private_include")
endif()

if(CONFIG_CAMERA_OV5645)
    list(APPEND srcs "sensors/ov5645/ov5645.c")
    list(APPEND include_dirs "sensors/ov5645/include")
    list(APPEND priv_include_dirs "sensors/ov5645/private_include")
endif()

if(CONFIG_CAMERA_OV5647)
    list(APPEND srcs "sensors/ov5647/ov5647.c")
    list(APPEND include_dirs "sensors/ov5647/include")
    list(APPEND priv_include_dirs "sensors/ov5647/private_include")
endif()

if(CONFIG_CAMERA_SC2336)
    list(APPEND srcs "sensors/sc2336/sc2336.c")
    list(APPEND include_dirs "sensors/sc2336/include")
    list(APPEND priv_include_dirs "sensors/sc2336/private_include")
endif()

if(CONFIG_CAMERA_SC202CS)
    list(APPEND srcs "sensors/sc202cs/sc202cs.c")
    list(APPEND include_dirs "sensors/sc202cs/include")
    list(APPEND priv_include_dirs "sensors/sc202cs/private_include")
endif()

if(CONFIG_CAMERA_GC0308)
    list(APPEND srcs "sensors/gc0308/gc0308.c")
    list(APPEND include_dirs "sensors/gc0308/include")
    list(APPEND priv_include_dirs "sensors/gc0308/private_include")
endif()

if(CONFIG_CAMERA_GC2145)
    list(APPEND srcs "sensors/gc2145/gc2145.c")
    list(APPEND include_dirs "sensors/gc2145/include")
    list(APPEND priv_include_dirs "sensors/gc2145/private_include")
endif()

if(CONFIG_CAMERA_SC101IOT)
    list(APPEND srcs "sensors/sc101iot/sc101iot.c")
    list(APPEND include_dirs "sensors/sc101iot/include")
    list(APPEND priv_include_dirs "sensors/sc101iot/private_include")
endif()

if(CONFIG_CAMERA_SC030IOT)
    list(APPEND srcs "sensors/sc030iot/sc030iot.c")
    list(APPEND include_dirs "sensors/sc030iot/include")
    list(APPEND priv_include_dirs "sensors/sc030iot/private_include")
endif()

if(CONFIG_CAMERA_BF3901)
    list(APPEND srcs "sensors/bf3901/bf3901.c")
    list(APPEND include_dirs "sensors/bf3901/include")
    list(APPEND priv_include_dirs "sensors/bf3901/private_include")
endif()

if(CONFIG_CAMERA_BF3925)
    list(APPEND srcs "sensors/bf3925/bf3925.c")
    list(APPEND include_dirs "sensors/bf3925/include")
    list(APPEND priv_include_dirs "sensors/bf3925/private_include")
endif()

if(CONFIG_CAMERA_BF3A03)
    list(APPEND srcs "sensors/bf3a03/bf3a03.c")
    list(APPEND include_dirs "sensors/bf3a03/include")
    list(APPEND priv_include_dirs "sensors/bf3a03/private_include")
endif()

if(CONFIG_CAMERA_MOTOR_DW9714)
    list(APPEND srcs "motors/dw9714/dw9714.c")
    list(APPEND include_dirs "motors/dw9714/include")
    list(APPEND priv_include_dirs "motors/dw9714/private_include")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${include_dirs}
                       PRIV_INCLUDE_DIRS ${priv_include_dirs}
                       REQUIRES ${requires}
                       PRIV_REQUIRES ${priv_requires}
                       LDFRAGMENTS linker.lf
                       WHOLE_ARCHIVE
                       )
