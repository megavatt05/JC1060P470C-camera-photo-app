/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * CamBrowser HTML processing: search-engine result extraction and
 * "page -> plain text" conversion.
 *
 * This is a deliberately small, resilient extractor, not a real HTML
 * parser: it never allocates (works in place on a mutable copy), it
 * tolerates malformed input, and it degrades to "?" for non-ASCII
 * (the 8x8 font is ASCII-only, see docs/ETHERNET_BROWSER.md).
 */

#include <string.h>
#include <stdio.h>
#include <strings.h>
#include <ctype.h>
#include "browser/html_text.h"
#include "browser/web_client.h"

static const char *ci_strstr(const char *hay, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0) {
        return hay;
    }
    for (const char *p = hay; *p != '\0'; p++) {
        size_t i = 0;
        while (i < nlen && p[i] != '\0' &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) {
            return p;
        }
    }
    return NULL;
}

/* Decode a small set of named entities plus numeric references.
 * Advances *pp past the entity; returns the decoded char (or '?' for
 * anything we cannot render with the ASCII font). */
static char decode_entity(const char **pp)
{
    const char *p = *pp;    /* p points at '&' */
    static const struct { const char *name; char ch; } named[] = {
        { "amp;",  '&' }, { "lt;",   '<' }, { "gt;",   '>' },
        { "quot;", '"' }, { "apos;", '\''}, { "nbsp;", ' ' },
        { "copy;", 'c' }, { "ndash;",'-' }, { "mdash;",'-' },
        { "hellip;", '.'}, { "laquo;", '<'}, { "raquo;", '>'},
        { "rsquo;", '\''}, { "lsquo;", '\''}, { "ldquo;", '"' }, { "rdquo;", '"' },
    };

    if (p[1] == '#') {
        long code = 0;
        int digits = 0;
        if (p[2] == 'x' || p[2] == 'X') {
            for (const char *q = p + 3; isxdigit((unsigned char)*q) && digits < 6; q++, digits++) {
                int v = (*q <= '9') ? *q - '0' : (tolower((unsigned char)*q) - 'a' + 10);
                code = code * 16 + v;
            }
        } else {
            for (const char *q = p + 2; isdigit((unsigned char)*q) && digits < 6; q++, digits++) {
                code = code * 10 + (*q - '0');
            }
        }
        const char *semi = strchr(p, ';');
        if (digits > 0 && semi != NULL) {
            *pp = semi + 1;
            if (code >= 0x20 && code <= 0x7E) {
                return (char)code;
            }
            return '?';
        }
        *pp = p + 1;
        return '&';
    }

    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        size_t nlen = strlen(named[i].name);
        if (strncasecmp(p + 1, named[i].name, nlen) == 0) {
            *pp = p + 1 + nlen;
            return named[i].ch;
        }
    }

    const char *semi = strchr(p, ';');
    if (semi != NULL && semi - p < 12) {
        *pp = semi + 1;
        return '?';     /* named entity we do not render */
    }
    *pp = p + 1;
    return '&';
}

/* Copy text, stripping tags and decoding entities, collapsing whitespace.
 * Returns the number of chars written. */
static size_t copy_visible(const char *begin, const char *end,
                           char *dst, size_t dst_size)
{
    size_t o = 0;
    bool space = true;  /* suppress leading whitespace */

    for (const char *p = begin; p < end && *p != '\0' && o + 1 < dst_size; ) {
        if (*p == '<') {
            const char *close = strchr(p, '>');
            if (close == NULL) {
                break;
            }
            p = close + 1;
            space = false;      /* a tag boundary may separate two words */
            continue;
        }
        if (*p == '&') {
            char c = decode_entity(&p);
            if (c == ' ' && space) {
                continue;
            }
            dst[o++] = c;
            space = (c == ' ');
            continue;
        }
        if (isspace((unsigned char)*p)) {
            if (!space) {
                dst[o++] = ' ';
                space = true;
            }
            p++;
            continue;
        }
        if (*p >= 0x20 && *p <= 0x7E) {
            dst[o++] = *p;
        } else {
            dst[o++] = '?';     /* non-ASCII placeholder */
        }
        space = false;
        p++;
    }

    while (o > 0 && dst[o - 1] == ' ') {
        o--;
    }
    dst[o] = '\0';
    return o;
}

/* Remove <script>...</script> and <style>...</style> blocks in place.
 * Returns the (possibly shrunk) new length. */
static size_t strip_blocks(char *s, size_t len)
{
    static const char *blocks[] = { "script", "style", "svg", "noscript" };

    for (size_t b = 0; b < sizeof(blocks) / sizeof(blocks[0]); b++) {
        char open[16], close[16];
        snprintf(open, sizeof(open), "<%s", blocks[b]);
        snprintf(close, sizeof(close), "</%s", blocks[b]);

        char *w = s;
        const char *r = s;
        const char *end = s + len;

        while (r < end) {
            const char *hit = ci_strstr(r, open);
            if (hit == NULL || hit >= end) {
                memmove(w, r, (size_t)(end - r));
                w += end - r;
                break;
            }
            /* copy up to the block */
            memmove(w, r, (size_t)(hit - r));
            w += hit - r;

            const char *close_tag = ci_strstr(hit, close);
            const char *resume = (close_tag != NULL) ?
                                 strchr(close_tag, '>') : NULL;
            r = (resume != NULL) ? resume + 1 : end;
        }
        len = (size_t)(w - s);
        s[len] = '\0';
    }
    return len;
}

/* Returns >0 when the tag starting at s is a block-level element whose
 * end should produce a newline in the text view. */
static int block_tag_newline(const char *s)
{
    static const char *tags[] = { "br", "p", "div", "h1", "h2", "h3",
                                  "h4", "h5", "h6", "li", "tr", "table"
                                };
    if (*s != '<') {
        return 0;
    }
    const char *name = (*s == '<' && s[1] == '/') ? s + 2 : s + 1;
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
        const char *t = tags[i];
        size_t nlen = strlen(t);
        if (strncasecmp(name, t, nlen) == 0) {
            char next = name[nlen];
            if (next == ' ' || next == '>' || next == '/' || next == '\t' || next == '\n') {
                return 1;
            }
        }
    }
    return 0;
}

/* --- Public API --------------------------------------------------------- */

esp_err_t html_extract_title(char *html, char *out, size_t out_size)
{
    if (html == NULL || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    const char *t = ci_strstr(html, "<title");
    if (t == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    const char *gt = strchr(t, '>');
    if (gt == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    const char *close = ci_strstr(gt, "</title");
    if (close == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    copy_visible(gt + 1, close, out, out_size);
    return (out[0] != '\0') ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* Find the closing '>' of an <a ...> tag that contains the attribute
 * pointed to by attr, then return the anchor text extent. */
static bool anchor_text_extent(const char *href_ptr,
                               const char **text_begin, const char **text_end)
{
    const char *gt = strchr(href_ptr, '>');
    if (gt == NULL) {
        return false;
    }
    *text_begin = gt + 1;
    const char *close = ci_strstr(gt, "</a>");
    if (close == NULL) {
        return false;
    }
    *text_end = close;
    return true;
}

/* Resolve a SERP href into a plain destination URL.
 * DDG wraps destinations as //duckduckgo.com/l/?uddg=<encoded>&rut=...
 * Ads link to /y.js — filtered out here. */
static bool resolve_href(const char *href, size_t href_len,
                         char *out, size_t out_size)
{
    char raw[512];
    if (href_len >= sizeof(raw)) {
        href_len = sizeof(raw) - 1;
    }
    memcpy(raw, href, href_len);
    raw[href_len] = '\0';

    if (strstr(raw, "duckduckgo.com/y.js") != NULL) {
        return false;   /* ad */
    }

    const char *uddg = strstr(raw, "uddg=");
    if (uddg != NULL) {
        /* Cut at the first raw '&' (next query param, raw or &amp;-escaped);
         * the destination itself carries only %XX escapes, so this is safe. */
        const char *amp = strchr(uddg + 5, '&');
        size_t uddg_len = (amp != NULL) ? (size_t)(amp - (uddg + 5)) : strlen(uddg + 5);
        char enc[512];
        if (uddg_len >= sizeof(enc)) {
            uddg_len = sizeof(enc) - 1;
        }
        memcpy(enc, uddg + 5, uddg_len);
        enc[uddg_len] = '\0';
        return web_url_decode(enc, out, out_size);
    }

    if (href_len >= out_size) {
        href_len = out_size - 1;
    }
    memcpy(out, href, href_len);
    out[href_len] = '\0';
    /* Force scheme-less protocol-relative URLs ("//host/path") to https */
    if (strncmp(out, "//", 2) == 0) {
        size_t olen = strlen(out);
        if (olen + 7 < out_size) {
            memmove(out + 7, out, olen + 1);
            memcpy(out, "https:", 6);
        }
    }
    return true;
}

/* Guarantee forward progress: never let the scan pointer move backwards
 * (a malformed/overlapping document could otherwise make the loop spin
 * forever on the same position). */
static const char *advance_past(const char *p, const char *candidate)
{
    const char *next = (candidate > p) ? candidate : p + 1;
    return next;
}

/* Generic DDG/Google-compatible anchor collector:
 * - ddg:    anchors with class="result__a"
 * - google: <h3> nested inside <a href> */
static int parse_serp(char *html, web_result_t *results, int max_results,
                      bool google_mode)
{
    strip_blocks(html, strlen(html));

    int count = 0;
    const char *p = html;

    while (count < max_results) {
        const char *href_ptr = NULL;
        const char *resume = NULL;

        if (!google_mode) {
            const char *anchor = ci_strstr(p, "class=\"result__a\"");
            if (anchor == NULL) {
                break;
            }
            /* back up to the '<a' that owns this class attribute */
            const char *tag = anchor;
            while (tag > html && *tag != '<') {
                tag--;
            }
            if (*tag != '<') {
                p = advance_past(p, anchor + 17);
                continue;
            }
            href_ptr = ci_strstr(tag, "href=\"");
            if (href_ptr == NULL || href_ptr > anchor + 64) {
                p = advance_past(p, anchor + 17);
                continue;
            }
        } else {
            const char *h3 = ci_strstr(p, "<h3");
            if (h3 == NULL) {
                break;
            }
            /* enclosing <a href=...> sits before the h3 */
            const char *window_start = (h3 - html > 2048) ? h3 - 2048 : html;
            const char *a = NULL;
            for (const char *q = h3; q > window_start; q--) {
                if (*q == '<' && q[1] == 'a' && (q[2] == ' ' || q[2] == '>')) {
                    a = q;
                    break;
                }
            }
            if (a == NULL) {
                p = advance_past(p, h3 + 3);
                continue;
            }
            href_ptr = ci_strstr(a, "href=\"");
            if (href_ptr == NULL || href_ptr >= h3) {
                p = advance_past(p, h3 + 3);
                continue;
            }
        }

        const char *href = href_ptr + 6;
        const char *href_end = strchr(href, '"');
        if (href_end == NULL) {
            break;
        }

        const char *text_begin, *text_end;
        if (!anchor_text_extent(href_ptr, &text_begin, &text_end)) {
            p = advance_past(p, href_end + 1);
            continue;
        }

        /* All skip paths below resume after the anchor text; advance_past()
         * makes progress guaranteed even when a malformed document places
         * the anchor end before the current scan position. */
        resume = advance_past(p, text_end + 1);

        char url[256];
        if (!resolve_href(href, (size_t)(href_end - href), url, sizeof(url))) {
            p = resume;
            continue;
        }

        /* Google internal links / junk filters */
        if (strstr(url, "/search?") == url ||
            strstr(url, "accounts.google.com") == url ||
            strstr(url, "translate.google.com") == url ||
            strstr(url, "duckduckgo.com/y.js") == url) {
            p = resume;
            continue;
        }

        char title[WEB_RESULT_TITLE_MAX];
        copy_visible(text_begin, text_end, title, sizeof(title));

        if (title[0] == '\0' || url[0] == '\0') {
            p = resume;
            continue;
        }

        strlcpy(results[count].url, url, sizeof(results[count].url));
        strlcpy(results[count].title, title, sizeof(results[count].title));
        count++;

        p = resume;
    }

    return count;
}

int html_parse_serp_ddg(char *html, web_result_t *results, int max_results)
{
    return parse_serp(html, results, max_results, false);
}

int html_parse_serp_google(char *html, web_result_t *results, int max_results)
{
    return parse_serp(html, results, max_results, true);
}

esp_err_t html_to_text(char *html, char *out, size_t out_size)
{
    if (html == NULL || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    strip_blocks(html, strlen(html));

    /* Skip the whole <head> (title/meta/style noise) if a body is present */
    const char *body = ci_strstr(html, "<body");
    if (body != NULL) {
        const char *body_gt = strchr(body, '>');
        if (body_gt != NULL) {
            html = (char *)(body_gt + 1);
        }
    }

    /* Generic strip into out: tags dropped, block tags emit a newline,
     * entities decoded, whitespace collapsed (newlines preserved). */
    size_t o = 0;
    const char *p = html;

    while (*p != '\0' && o + 1 < out_size) {
        if (*p == '<') {
            bool nl = block_tag_newline(p) != 0;
            const char *gt = strchr(p, '>');
            if (gt == NULL) {
                break;
            }
            p = gt + 1;
            if (nl && o > 0 && out[o - 1] != '\n' && out[o - 1] != ' ') {
                out[o++] = '\n';
            }
            continue;
        }
        if (*p == '&') {
            out[o++] = decode_entity(&p);
            continue;
        }
        if (*p == '\n') {
            if (o > 0 && out[o - 1] != '\n' && out[o - 1] != ' ') {
                out[o++] = '\n';
            }
            p++;
            continue;
        }
        if (isspace((unsigned char)*p)) {
            if (o > 0 && out[o - 1] != ' ' && out[o - 1] != '\n') {
                out[o++] = ' ';
            }
            p++;
            continue;
        }
        out[o++] = (*p >= 0x20 && *p <= 0x7E) ? *p : '?';
        p++;
    }

    /* Trim trailing spaces/newlines */
    while (o > 0 && (out[o - 1] == '\n' || out[o - 1] == ' ')) {
        o--;
    }
    out[o] = '\0';
    return ESP_OK;
}
