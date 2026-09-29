#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# =============================================================================
# patch_idf6_managed_components.py
#
# Патч managed_components для сборки JC1060P470C-camera-photo-app на ESP-IDF 6.0.x.
# Кроссплатформенная замена tools/patch_esp_ipa_idf6.ps1 (Linux / Windows / macOS).
#
# ЗАПУСК (после idf.py set-target esp32p4, когда managed_components/ создан):
#     python patch_idf6_managed_components.py          # Windows (python из IDF-окружения)
#     python3 patch_idf6_managed_components.py         # Linux / macOS
#     python patch_idf6_managed_components.py --check  # только проверка, без изменений
#
# После патча: idf.py build
# Скрипт идемпотентен: повторный запуск безопасен. Запускать заново после каждого
# idf.py set-target / fullclean, заново скачивающего managed_components.
#
# Состав патчей (см. docs/BUILD_IDF6.md):
#  P1. esp_ipa:            include hal/isp_types.h -> driver/isp_types.h
#  P2. esp_ipa:            REQUIRES esp_driver_isp (include-путь для P1; без него —
#                          fatal error: driver/isp_types.h: No such file or directory)
#  P3. esp_video:          PRIV_REQUIRES += esp_driver_gpio, esp_driver_i2c
#  P4. esp_video isp:      case CAM_CTLR_COLOR_YUV422 -> 4 варианта (YUYV/UYVY/YVYU/VYUY);
#                          COLOR_SPACE_TYPE()==COLOR_SPACE_RAW -> COLOR_SPACE_TYPE_IS_RAW()
#  P5. esp_video csi/dvp/spi: CAM_CTLR_COLOR_YUV422 -> CAM_CTLR_COLOR_YUV422_YUYV
#  P6. esp_lcd_jd9365:     panel_dev_config->color_space -> rgb_ele_order
#
# Выход: 0 — все патчи на месте (применены или уже были);
#        1 — есть ошибки (список в выводе) или нет managed_components.
# =============================================================================
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
MC = os.path.join(ROOT, "managed_components")

CHECK_ONLY = "--check" in sys.argv

RESULTS = []  # (status, rel, desc)

MARK = {
    "PATCHED": "[ИСПРАВЛЕНО]",
    "ALREADY": "[УЖЕ БЫЛО]",
    "FAILED":  "[ОШИБКА]  ",
    "MISS":    "[НЕТ ФАЙЛА]",
}


def record(status, rel, desc):
    RESULTS.append((status, rel, desc))
    print("  %s %s: %s" % (MARK[status], rel, desc))


def load(rel):
    path = os.path.join(MC, rel)
    if not os.path.isfile(path):
        return None
    with io.open(path, "r", encoding="utf-8", errors="surrogateescape", newline="") as f:
        return f.read()


def save(rel, content):
    path = os.path.join(MC, rel)
    with io.open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
        f.write(content)


def replace_str(rel, old, new, desc):
    """Замена подстроки. ALREADY, если new уже присутствует, а old отсутствует."""
    c = load(rel)
    if c is None:
        record("MISS", rel, desc)
        return False
    if old in c:
        if CHECK_ONLY:
            record("FAILED", rel, desc + " — ТРЕБУЕТСЯ ПАТЧ (режим --check)")
            return False
        save(rel, c.replace(old, new))
        record("PATCHED", rel, desc)
        return True
    if new in c:
        record("ALREADY", rel, desc)
        return True
    record("FAILED", rel, desc + " — паттерн не найден")
    return False


def find_matching_paren(text, open_idx):
    """Индекс закрывающей скобки для открывающей в open_idx (учитывает "строки")."""
    depth = 0
    i = open_idx
    in_str = False
    while i < len(text):
        ch = text[i]
        if in_str:
            if ch == '"':
                in_str = False
        else:
            if ch == '"':
                in_str = True
            elif ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    return i
        i += 1
    return -1


def cmake_add_deps(rel, keyword, deps, desc):
    """Добавить deps в REQUIRES/PRIV_REQUIRES компонента.

    Вариант A: set(priv_requires "a" "b") / set(requires "a" ...)
    Вариант B: ключевое слово KEYWORD внутри idf_component_register(...)
    Вариант C: вставка 'KEYWORD dep1 dep2' перед закрывающей скобкой
               idf_component_register(...)
    Работает при любом форматировании CMakeLists.txt.
    """
    c = load(rel)
    if c is None:
        record("MISS", rel, desc)
        return False
    missing = [d for d in deps if d not in c]
    if not missing:
        record("ALREADY", rel, desc)
        return True
    if CHECK_ONLY:
        record("FAILED", rel, desc + " — ТРЕБУЕТСЯ ПАТЧ (режим --check)")
        return False

    # Вариант A: set(<keyword lowercase> ...)
    var_re = re.compile(r"set\s*\(\s*%s\b" % keyword.lower(), re.IGNORECASE)
    m = var_re.search(c)
    if m:
        open_idx = c.find("(", m.start())
        close = find_matching_paren(c, open_idx)
        if close > 0:
            additions = "".join(' "%s"' % d for d in missing)
            c = c[:close] + additions + c[close:]
            save(rel, c)
            record("PATCHED", rel, desc)
            return True

    # Вариант B/C: внутри idf_component_register(...)
    reg = re.search(r"idf_component_register\s*\(", c)
    if reg:
        open_idx = reg.end() - 1
        close = find_matching_paren(c, open_idx)
        if close > 0:
            span = c[open_idx:close]
            kw = re.search(r"\b%s\b" % keyword, span)
            if kw:
                insert_at = open_idx + kw.end()
                c = c[:insert_at] + " " + " ".join(missing) + c[insert_at:]
            else:
                indent = " " * 23
                c = c[:close] + "\n" + indent + keyword + " " + " ".join(missing) + c[close:]
            save(rel, c)
            record("PATCHED", rel, desc)
            return True

    record("FAILED", rel, desc + " — не найден idf_component_register/set(%s)" % keyword.lower())
    return False


IPA_TYPES = os.path.join("espressif__esp_ipa", "include", "esp_ipa_types.h")
IPA_CMAKE = os.path.join("espressif__esp_ipa", "CMakeLists.txt")
VIDEO_CMAKE = os.path.join("espressif__esp_video", "CMakeLists.txt")
ISP_DEVICE = os.path.join("espressif__esp_video", "src", "device", "esp_video_isp_device.c")
YUV_FILES = [
    os.path.join("espressif__esp_video", "src", "device", "esp_video_csi_device.c"),
    os.path.join("espressif__esp_video", "src", "device", "esp_video_dvp_device.c"),
    os.path.join("espressif__esp_video", "src", "device", "esp_video_spi_device.c"),
]
JD9365 = os.path.join("espressif__esp_lcd_jd9365", "esp_lcd_jd9365.c")


def patch_isp_device():
    """P4: esp_video_isp_device.c — расширение case YUV422 + замена COLOR_SPACE_TYPE."""
    rel = ISP_DEVICE
    c = load(rel)
    if c is None:
        record("MISS", rel, "isp_device: YUV422 case + COLOR_SPACE_TYPE")
        return False
    ok = True
    changed = False

    # 4a. Один case -> четыре (IDF 6 разделил CAM_CTLR_COLOR_YUV422 на YUYV/UYVY/YVYU/VYUY)
    old_case = "    case CAM_CTLR_COLOR_YUV422:\n        *isp_color = ISP_COLOR_YUV422;"
    new_case = ("    case CAM_CTLR_COLOR_YUV422_YUYV:\n"
                "    case CAM_CTLR_COLOR_YUV422_UYVY:\n"
                "    case CAM_CTLR_COLOR_YUV422_YVYU:\n"
                "    case CAM_CTLR_COLOR_YUV422_VYUY:\n"
                "        *isp_color = ISP_COLOR_YUV422;")
    if old_case in c:
        c = c.replace(old_case, new_case)
        changed = True
    elif "CAM_CTLR_COLOR_YUV422_YUYV:" not in c:
        record("FAILED", rel, "case CAM_CTLR_COLOR_YUV422 не найден")
        ok = False

    # 4b. Макрос-замена COLOR_SPACE_TYPE()/COLOR_SPACE_RAW (удалены в IDF 6)
    anchor = '#include "soc/isp_struct.h"'
    macro_def = (
        "\n\n"
        "/*\n"
        " * ESP-IDF 6.0: макросы COLOR_SPACE_TYPE и COLOR_SPACE_RAW удалены.\n"
        " * Цветовые enum'ы теперь FourCC; Bayer-RAW начинается с байтов 'R','A','W'.\n"
        " */\n"
        "#define COLOR_SPACE_TYPE_IS_RAW(color) \\\n"
        "    (((uint32_t)(color) & 0x00FFFFFF) == "
        "((uint32_t)ESP_COLOR_FOURCC('R', 'A', 'W', 0) & 0x00FFFFFF))\n"
    )
    if "COLOR_SPACE_TYPE_IS_RAW" not in c:
        if anchor in c:
            c = c.replace(anchor, anchor + macro_def, 1)
            changed = True
        else:
            record("FAILED", rel, "якорь #include \"soc/isp_struct.h\" не найден")
            ok = False

    # 4c. Замены вызовов
    c, n_eq = re.subn(r"COLOR_SPACE_TYPE\(([^)]*)\) == COLOR_SPACE_RAW",
                      r"COLOR_SPACE_TYPE_IS_RAW(\1)", c)
    c, n_ne = re.subn(r"COLOR_SPACE_TYPE\(([^)]*)\) != COLOR_SPACE_RAW",
                      r"(!COLOR_SPACE_TYPE_IS_RAW(\1))", c)
    if n_eq + n_ne:
        changed = True
    leftover = c.replace("COLOR_SPACE_TYPE_IS_RAW(", "").find("COLOR_SPACE_TYPE(")
    if leftover != -1:
        record("FAILED", rel, "остались незамещённые вызовы COLOR_SPACE_TYPE()")
        ok = False

    if not ok:
        return False
    if CHECK_ONLY:
        # в режиме проверки сообщаем, применён ли патч целиком
        fully = ("CAM_CTLR_COLOR_YUV422_YUYV:" in c and "COLOR_SPACE_TYPE_IS_RAW" in c
                 and leftover == -1)
        record("ALREADY" if fully else "FAILED",
               rel, "isp_device: YUV422 case + COLOR_SPACE_TYPE"
               + ("" if fully else " — ТРЕБУЕТСЯ ПАТЧ (режим --check)"))
        return fully
    if changed:
        save(rel, c)
    record("PATCHED" if changed else "ALREADY", rel,
           "isp_device: YUV422 case + COLOR_SPACE_TYPE")
    return True


def patch_yuv_generic():
    """P5: CAM_CTLR_COLOR_YUV422 -> CAM_CTLR_COLOR_YUV422_YUYV в csi/dvp/spi."""
    total_ok = True
    any_patched = False
    any_already = False
    for rel in YUV_FILES:
        c = load(rel)
        if c is None:
            record("MISS", rel, "CAM_CTLR_COLOR_YUV422 -> _YUYV")
            total_ok = False
            continue
        c2, n = re.subn(r"\bCAM_CTLR_COLOR_YUV422\b(?!_)", "CAM_CTLR_COLOR_YUV422_YUYV", c)
        if n > 0:
            if CHECK_ONLY:
                record("FAILED", rel, "CAM_CTLR_COLOR_YUV422 -> _YUYV — ТРЕБУЕТСЯ ПАТЧ (режим --check)")
                total_ok = False
                continue
            save(rel, c2)
            record("PATCHED", rel, "CAM_CTLR_COLOR_YUV422 -> _YUYV (%d шт.)" % n)
            any_patched = True
        elif "CAM_CTLR_COLOR_YUV422_YUYV" in c:
            record("ALREADY", rel, "CAM_CTLR_COLOR_YUV422 -> _YUYV")
            any_already = True
        else:
            record("FAILED", rel, "CAM_CTLR_COLOR_YUV422 не найден")
            total_ok = False
    return total_ok


def main():
    print("=" * 74)
    print("patch_idf6_managed_components.py — патч managed_components для ESP-IDF 6.0.x")
    print("Каталог проекта: %s" % ROOT)
    if CHECK_ONLY:
        print("РЕЖИМ: --check (только проверка, файлы не изменяются)")
    print("=" * 74)

    if not os.path.isdir(MC):
        print("\nОШИБКА: каталог managed_components/ не найден.")
        print("Сначала выполните: idf.py set-target esp32p4")
        return 1

    print("\n[1/6] esp_ipa: заголовок isp_types.h")
    ok1 = replace_str(IPA_TYPES,
                      '#include "hal/isp_types.h"',
                      '#include "driver/isp_types.h"',
                      "hal/isp_types.h -> driver/isp_types.h")

    print("\n[2/6] esp_ipa: зависимость esp_driver_isp (без неё P1 не работает!)")
    ok2 = cmake_add_deps(IPA_CMAKE, "REQUIRES", ["esp_driver_isp"],
                         "REQUIRES += esp_driver_isp")

    print("\n[3/6] esp_video: зависимости gpio/i2c")
    ok3 = cmake_add_deps(VIDEO_CMAKE, "PRIV_REQUIRES",
                         ["esp_driver_gpio", "esp_driver_i2c"],
                         "PRIV_REQUIRES += esp_driver_gpio, esp_driver_i2c")

    print("\n[4/6] esp_video: isp_device (case YUV422 + COLOR_SPACE_TYPE)")
    ok4 = patch_isp_device()

    print("\n[5/6] esp_video: csi/dvp/spi (CAM_CTLR_COLOR_YUV422)")
    ok5 = patch_yuv_generic()

    print("\n[6/6] esp_lcd_jd9365: поле конфигурации панели")
    ok6 = replace_str(JD9365,
                      "switch (panel_dev_config->color_space) {",
                      "switch (panel_dev_config->rgb_ele_order) {",
                      "panel_dev_config->color_space -> rgb_ele_order")

    failed = [r for r in RESULTS if r[0] in ("FAILED", "MISS")]
    patched = len([r for r in RESULTS if r[0] == "PATCHED"])
    already = len([r for r in RESULTS if r[0] == "ALREADY"])

    print("\n" + "=" * 74)
    print("ИТОГ: исправлено %d, уже было %d, ошибок %d" % (patched, already, len(failed)))
    if failed:
        print("\nПроблемы:")
        for status, rel, desc in failed:
            print("  - %s: %s" % (rel, desc))
        print("\nПроверьте версии компонентов (dependencies.lock) и целостность")
        print("managed_components/. При сомнениях:")
        print("  rm -rf build managed_components sdkconfig")
        print("  idf.py set-target esp32p4")
        print("  python patch_idf6_managed_components.py")
        return 1
    if not CHECK_ONLY:
        print("\nВсе патчи на месте. Теперь: idf.py build")
    return 0


if __name__ == "__main__":
    sys.exit(main())
