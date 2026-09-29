#!/usr/bin/env bash
# =============================================================================
# patch_idf6_managed_components.sh
# Полный набор патчей managed_components для сборки JC1060P470C-camera-photo-app
# на ESP-IDF 6.0.x. Это Linux-расширение tools/patch_esp_ipa_idf6.ps1: помимо
# замены hal/isp_types.h -> driver/isp_types.h добавлены все фиксы, выявленные
# при верификации компиляции на IDF 6.0.3.
#
# КОГДА ЗАПУСКАТЬ: после idf.py set-target esp32p4 (или idf.py reconfigure),
# когда каталог managed_components/ уже создан. Скрипт идемпотентен.
#
# Состав патчей:
#  1. esp_ipa:            include hal/isp_types.h -> driver/isp_types.h
#  2. esp_ipa:            REQUIRES esp_driver_isp (include-путь для п.1)
#  3. esp_video:          esp_driver_gpio + esp_driver_i2c в PRIV_REQUIRES
#  4. esp_video csi/dvp:  CAM_CTLR_COLOR_YUV422 -> CAM_CTLR_COLOR_YUV422_YUYV
#  5. esp_video isp:      case YUV422 -> 4 варианта; COLOR_SPACE_TYPE ->
#                         COLOR_SPACE_TYPE_IS_RAW (FourCC-классификатор)
#  6. esp_lcd_jd9365:     panel_dev_config->color_space -> ->rgb_ele_order
#
# Патчи уровня репозитория (git): components/espressif__esp_cam_sensor/
# CMakeLists.txt (esp_driver_spi) и main/app_lcd.c (in/out_color_format,
# esp_lcd_dpi_panel_enable_dma2d) — применяются git-патчем и коммитятся.
# =============================================================================
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
MC="$ROOT/managed_components"
if [ ! -d "$MC" ]; then
  echo "ОШИБКА: $MC не найден. Сначала выполните: idf.py set-target esp32p4"
  exit 1
fi

python3 - "$MC" <<'PYEOF'
import io, os, re, sys

mc = sys.argv[1]

def patch(rel, fn):
    path = os.path.join(mc, rel)
    if not os.path.exists(path):
        print(f"  [MISS]  {rel}")
        return
    with io.open(path, encoding="utf-8") as f:
        c = f.read()
    c2 = fn(c)
    if c2 != c:
        with io.open(path, "w", encoding="utf-8") as f:
            f.write(c2)
        print(f"  [PATCHED] {rel}")
    else:
        print(f"  [OK]      {rel}")

print("--- 1-2. espressif__esp_ipa ---")
patch(os.path.join("espressif__esp_ipa", "include", "esp_ipa_types.h"),
      lambda c: c.replace('#include "hal/isp_types.h"', '#include "driver/isp_types.h"'))

def ipa_cmake(c):
    if "REQUIRES esp_driver_isp" in c:
        return c
    return c.replace(
        "LDFRAGMENTS linker.lf)",
        "LDFRAGMENTS linker.lf\n"
        "                       REQUIRES esp_driver_isp)")
patch(os.path.join("espressif__esp_ipa", "CMakeLists.txt"), ipa_cmake)

print("--- 3. espressif__esp_video: CMakeLists ---")
def video_cmake(c):
    if "esp_driver_gpio" in c:
        return c
    return c.replace('set(priv_requires "vfs")',
                     'set(priv_requires "vfs" "esp_driver_gpio" "esp_driver_i2c")')
patch(os.path.join("espressif__esp_video", "CMakeLists.txt"), video_cmake)

print("--- 4. espressif__esp_video: csi/dvp YUV422 ---")
def yuv422(c):
    return c.replace("CAM_CTLR_COLOR_YUV422;", "CAM_CTLR_COLOR_YUV422_YUYV;")
patch(os.path.join("espressif__esp_video", "src", "device", "esp_video_csi_device.c"), yuv422)
patch(os.path.join("espressif__esp_video", "src", "device", "esp_video_dvp_device.c"), yuv422)

print("--- 5. espressif__esp_video: isp_device ---")
def isp_device(c):
    old_case = "    case CAM_CTLR_COLOR_YUV422:\n        *isp_color = ISP_COLOR_YUV422;"
    new_case = ("    case CAM_CTLR_COLOR_YUV422_YUYV:\n"
                "    case CAM_CTLR_COLOR_YUV422_UYVY:\n"
                "    case CAM_CTLR_COLOR_YUV422_YVYU:\n"
                "    case CAM_CTLR_COLOR_YUV422_VYUY:\n"
                "        *isp_color = ISP_COLOR_YUV422;")
    c = c.replace(old_case, new_case)
    if "COLOR_SPACE_TYPE_IS_RAW" not in c:
        anchor = '#include "soc/isp_struct.h"'
        macro = anchor + "\n\n/*\n * ESP-IDF 6.0: COLOR_SPACE_TYPE()/COLOR_SPACE_RAW удалены.\n * Цветовые enum'ы теперь FourCC; Bayer-RAW начинается с байтов 'R','A','W'.\n */\n#define COLOR_SPACE_TYPE_IS_RAW(color) \\\n    (((uint32_t)(color) & 0x00FFFFFF) == ((uint32_t)ESP_COLOR_FOURCC('R', 'A', 'W', 0) & 0x00FFFFFF))\n"
        c = c.replace(anchor, macro, 1)
    c = re.sub(r"COLOR_SPACE_TYPE\(([^)]*)\) == COLOR_SPACE_RAW", r"COLOR_SPACE_TYPE_IS_RAW(\1)", c)
    c = re.sub(r"COLOR_SPACE_TYPE\(([^)]*)\) != COLOR_SPACE_RAW", r"(!COLOR_SPACE_TYPE_IS_RAW(\1))", c)
    return c
patch(os.path.join("espressif__esp_video", "src", "device", "esp_video_isp_device.c"), isp_device)

print("--- 6. espressif__esp_lcd_jd9365 ---")
patch(os.path.join("espressif__esp_lcd_jd9365", "esp_lcd_jd9365.c"),
      lambda c: c.replace("switch (panel_dev_config->color_space) {",
                          "switch (panel_dev_config->rgb_ele_order) {"))

print("=== Готово. Теперь: idf.py build ===")
PYEOF
