/* Host-only driver: renders CamBrowser screens (the real browser.c drawing
 * code) into BMP files so UI changes can be reviewed without hardware.
 * Built by run.sh with -DCAMOS_UI_PREVIEW; never compiled into firmware. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

#include "app_lcd.h"
#include "net/app_eth.h"
#include "camos/touch.h"   /* real header: types only */
#include "browser/films.h" /* film_item_t, FILMS_MAX */

/* --- impls for the stub headers ------------------------------------------ */

static uint16_t fb1[EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES];
static int frame_no = 0;
char g_bmp_out[512];

int app_lcd_get_fb(int idx, void **fb)
{
    (void)idx;
    *fb = fb1;
    return 0;
}

static void write_bmp24(const char *path, const uint16_t *px, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    int pad = (4 - (w * 3) % 4) % 4;
    uint32_t size = 54u + (uint32_t)(w * 3 + pad) * h;
    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &size, 4);
    uint32_t off = 54; memcpy(hdr + 10, &off, 4);
    uint32_t hs = 40;  memcpy(hdr + 14, &hs, 4);
    int32_t ww = w, hh = h; memcpy(hdr + 18, &ww, 4); memcpy(hdr + 22, &hh, 4);
    uint16_t planes = 1, bpp = 24; memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);
    fwrite(hdr, 1, 54, f);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint16_t v = px[(size_t)y * w + x];
            uint8_t r = ((v >> 11) & 0x1F) << 3;
            uint8_t g = ((v >> 5) & 0x3F) << 2;
            uint8_t b = (v & 0x1F) << 3;
            uint8_t bgr[3] = { b, g, r };
            fwrite(bgr, 1, 3, f);
        }
        for (int p = 0; p < pad; p++) {
            fputc(0, f);
        }
    }
    fclose(f);
}

int app_lcd_flush(int idx)
{
    (void)idx;
    char path[600];
    snprintf(path, sizeof(path), "%s_%02d.bmp", g_bmp_out, frame_no++);
    write_bmp24(path, fb1, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    return 0;
}

int vTaskDelay(int ticks) { (void)ticks; return 0; }
int xTaskCreatePinnedToCore(void *fn, const char *n, int s, void *a, int p, void *h, int c)
{ (void)fn; (void)n; (void)s; (void)a; (void)p; (void)h; (void)c; return 1; }

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size) {
        size_t n = (len >= size) ? size - 1 : len;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}

size_t strlcat(char *dst, const char *src, size_t size)
{
    size_t d = strlen(dst);
    if (d >= size) return d + strlen(src);
    return d + strlcpy(dst + d, src, size - d);
}

/* eth */
bool app_eth_ready(void) { return true; }
const char *app_eth_ip_str(void) { return "192.168.1.106"; }
const char *app_eth_status_str(void) { return "нет кабеля"; }
app_eth_state_t app_eth_state(void) { return APP_ETH_READY; }
esp_err_t app_eth_start(void) { return ESP_OK; }

/* touch */
int touch_init(void) { return 0; }
int touch_poll(touch_point_t *pts, int max) { (void)pts; (void)max; return 0; }
const char *touch_chip_name(void) { return "GT911"; }

/* media: state driven per-scene from main() */
int g_media_state = 0;         /* media_state_t */
int g_media_vol = 60;
char g_media_url[256] = "";

int media_get_state(void) { return g_media_state; }
const char *media_state_str(void)
{
    static const char *names[] = { "ожидание", "соединение", "играет", "пауза", "конец", "ошибка" };
    return names[g_media_state % 6];
}
int media_get_volume(void) { return g_media_vol; }
const char *media_get_url(void) { return g_media_url; }
bool media_video_active(void) { return false; }
esp_err_t media_radio_start(const char *url) { (void)url; return ESP_OK; }
esp_err_t media_video_start(const char *url) { (void)url; return ESP_OK; }
void media_pause(void) {}
void media_resume(void) {}
void media_stop(void) {}
void media_set_volume(int v) { g_media_vol = v; }
void sdcard_reprobe(void) {}
esp_err_t sdcard_mount(void) { return ESP_FAIL; }
const char *sdcard_mp(void) { return "/sdcard"; }

/* films: not exercised by the preview scenes */
esp_err_t films_search(const char *q, film_item_t *it, int max, int *out_n)
{ (void)q; (void)it; (void)max; *out_n = 0; return ESP_FAIL; }
films_play_err_t films_resolve(const char *id, char *url, size_t sz)
{ (void)id; (void)url; (void)sz; return FILMS_ERR_NET; }
films_play_err_t films_probe_url(const char *url, uint16_t *w, uint16_t *h)
{ (void)url; *w = *h = 0; return FILMS_ERR_NET; }
void films_set_prog_cb(films_prog_fn fn, void *ctx) { (void)fn; (void)ctx; }
const char *films_err_str(films_play_err_t e) { (void)e; return "ошибка"; }

/* --- preview driver ------------------------------------------------------- */

extern void ui_preview_set(int st, const char *q, int eng, int scroll,
                           const film_item_t *films, int nf, const char *msg,
                           int nres, const char *const *res_titles);
extern void ui_preview_page(const char *text);
extern void ui_preview_sd(const char *const *names, const int *video, int n, int err);
extern void ui_preview_busy(const char *title, const char *stage,
                            const char *extra, int elapsed_s);

enum { ST_SPLASH = 0, ST_HOME, ST_LOADING, ST_RESULTS, ST_PAGELOAD, ST_PAGE,
       ST_BUSY, ST_RADIO, ST_VIDEO, ST_URLIN, ST_VIDEOP, ST_FILES, ST_FILMS,
       ST_FILMSR };

static void scene(const char *name)
{
    snprintf(g_bmp_out, sizeof(g_bmp_out), "/home/z/my-project/download/ui_preview/%s", name);
}

int main(void)
{
    mkdir("/home/z/my-project/download/ui_preview", 0755);

    scene("01_splash");
    ui_preview_set(ST_SPLASH, NULL, 0, 0, NULL, 0, NULL, 0, NULL);

    scene("02_home_empty");
    ui_preview_set(ST_HOME, "", 0, 0, NULL, 0, NULL, 0, NULL);

    scene("03_home_typed");
    ui_preview_set(ST_HOME, "интернет радио", 0, 0, NULL, 0, NULL, 0, NULL);

    static const char *const titles[8] = {
        "Интернет-радио онлайн — слушать бесплатно",
        "Радио Рекорд — главный танцевальный",
        "Европа Плюс — официальный сайт",
        "SomaFM: Commercial-Free Internet Radio",
        "Как работает интернет-радио: статья",
        "Online Radio Box — все станции мира",
        "Радио Дача — слушать прямой эфир",
        "10 лучших радиостанций 2026 года",
    };
    scene("04_results");
    ui_preview_set(ST_RESULTS, "интернет радио", 0, 0, NULL, 0, NULL, 8, titles);

    static char page[8192];
    size_t off = 0;
    const char *paras[] = {
        "Интернет-радио — технология передачи аудиопотока через сеть.",
        "Первые станции появились в 1993 году и с тех пор стали одним из",
        "самых популярных способов прослушивания музыки на встраиваемых",
        "устройствах. Поток обычно кодируется MP3 или AAC и передаётся по",
        "протоколу HTTP: клиент подключается к адресу станции и читает",
        "данные непрерывно, воспроизводя их по мере поступления.",
        "",
        "Плеер на ESP32-P4 использует GMF-конвейер: элемент HTTP читает",
        "поток, декодер превращает сжатые данные в PCM, а кодек ES8311",
        "выводит звук на наушники или динамик. Громкость регулируется",
        "кнопками на экране или горячими клавишами.",
    };
    for (size_t i = 0; i < sizeof(paras) / sizeof(paras[0]); i++) {
        for (int rep = 0; rep < 2; rep++) {
            off += (size_t)snprintf(page + off, sizeof(page) - off, "%s ",
                                    paras[i]);
        }
        off += (size_t)snprintf(page + off, sizeof(page) - off, "\n");
    }
    scene("05_page");
    ui_preview_page(page);

    film_item_t films[6] = {
        { "Night of the Living Dead (1968)", "night-of-the-living-dead" },
        { "Nosferatu (1922)", "nosferatu" },
        { "The Cabinet of Dr. Caligari (1920)", "cabinet-of-dr-caligari" },
        { "His Girl Friday (1940)", "his-girl-friday" },
        { "Plan 9 from Outer Space (1957)", "plan-9-from-outer-space" },
        { "Charade (1963)", "charade-1963" },
    };
    scene("06_films_search");
    ui_preview_set(ST_FILMS, "ту", 0, 0, NULL, 0, NULL, 0, NULL);

    scene("07_films_list");
    ui_preview_set(ST_FILMSR, "ту", 0, 0, films, 6, "", 0, NULL);

    scene("08_films_error");
    ui_preview_set(ST_FILMSR, "ту", 0, 0, NULL, 0,
                   "сервер не ответил", 0, NULL);

    g_media_state = 2; /* PLAYING */
    strlcpy(g_media_url, "http://ep128server.streamr.ru:8030/ep128", sizeof(g_media_url));
    scene("09_radio");
    ui_preview_set(ST_RADIO, "", 0, 0, NULL, 0, NULL, 0, NULL);

    g_media_state = 0;
    g_media_url[0] = '\0';
    scene("10_video");
    ui_preview_set(ST_VIDEO, "", 0, 0, NULL, 0, NULL, 0, NULL);

    static const char *const fnames[5] = {
        "bbb_180p_cb.mp4", "nosferatu_1922.mp4", "lofi_beat.mp3",
        "audiobook_ch01.m4a", "jazz_night.mp3",
    };
    static const int fvideo[5] = { 1, 1, 0, 0, 0 };
    scene("11_files");
    ui_preview_sd(fnames, fvideo, 5, 0);

    scene("12_loading");
    ui_preview_set(ST_LOADING, "", 0, 0, NULL, 0, NULL, 0, NULL);

    /* live busy card (new): the phases a film goes through */
    scene("13_busy_metadata");
    ui_preview_busy("1955 - Crashout", "читаю метаданные", "", 4);

    scene("14_busy_probe");
    ui_preview_busy("1955 - Crashout", "проверяю файл",
                    "1.4 из 2.9 МБ, 19 кБ/с", 152);

    scene("15_busy_player");
    ui_preview_busy("видео", "готовлю плеер", "", 187);

    printf("rendered %d frames\n", frame_no);
    return 0;
}

/* web/html: only referenced by the interactive task, never run on host */
#include "browser/web_client.h"
#include "browser/html_text.h"
esp_err_t web_search(const char *q, bool google, char **body, size_t *len)
{ (void)q; (void)google; *body = NULL; *len = 0; return ESP_FAIL; }
esp_err_t web_get(const char *url, const char *extra_cookie, char **out_body,
                  size_t *out_len, int *out_status)
{ (void)url; (void)extra_cookie; *out_body = NULL; *out_len = 0;
  if (out_status) *out_status = 0; return ESP_FAIL; }
int html_parse_serp_ddg(char *h, web_result_t *r, int m) { (void)h; (void)r; (void)m; return 0; }
int html_parse_serp_google(char *h, web_result_t *r, int m) { (void)h; (void)r; (void)m; return 0; }
esp_err_t html_to_text(char *html, char *out, size_t sz) { (void)html; (void)sz; if (sz) out[0] = 0; return ESP_OK; }
esp_err_t html_extract_title(char *h, char *o, size_t s) { (void)h; (void)s; if (s) o[0] = 0; return ESP_OK; }
