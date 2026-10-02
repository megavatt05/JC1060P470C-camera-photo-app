#pragma once
#include "esp_err.h"

#include <stddef.h>

/* --- esp_http_client --------------------------------------------------- */
typedef struct esp_http_client_config {
    const char *url;
    int         buffer_size;
    int         buffer_size_tx;
    int         timeout_ms;
    int         max_redirection_count;
    void      (*crt_bundle_attach)(void *);
    int         keep_alive_enable;
} esp_http_client_config_t;

typedef void *esp_http_client_handle_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *k, const char *v);
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int write_len);
int       esp_http_client_fetch_headers(esp_http_client_handle_t c);
int       esp_http_client_get_status_code(esp_http_client_handle_t c);
int       esp_http_client_read(esp_http_client_handle_t c, char *buf, int len);
esp_err_t esp_http_client_close(esp_http_client_handle_t c);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c);

/* --- esp_crt_bundle ----------------------------------------------------- */
void esp_crt_bundle_attach(void *conf);


