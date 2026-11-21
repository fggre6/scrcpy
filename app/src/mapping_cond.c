#include "mapping_cond.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "mapping_manager.h"
#include "util/log.h"

// ============================================================
// Feature flag registry
// ============================================================

#define SC_COND_MAX_FLAGS 64

static struct sc_cond_flag_def g_flags[SC_COND_MAX_FLAGS];
static int g_flags_count;
static void *g_flags_ctx;

bool
sc_cond_flag_register(const char *name, const char *desc, sc_cond_flag_fn fn) {
    if (!name || !*name || !fn) {
        return false;
    }
    if (strlen(name) >= SC_COND_NAME_MAX) {
        LOGW("Condition flag name too long (max %d): %s", SC_COND_NAME_MAX - 1,
             name);
        return false;
    }
    if (sc_cond_flag_find(name)) {
        LOGW("Condition flag already registered: %s", name);
        return false;
    }
    if (g_flags_count >= SC_COND_MAX_FLAGS) {
        LOGW("Condition flag table full (%d), ignoring '%s'",
             SC_COND_MAX_FLAGS, name);
        return false;
    }
    struct sc_cond_flag_def *d = &g_flags[g_flags_count++];
    snprintf(d->name, sizeof(d->name), "%s", name);
    snprintf(d->desc, sizeof(d->desc), "%s", desc ? desc : "");
    d->fn = fn;
    return true;
}

const struct sc_cond_flag_def *
sc_cond_flag_find(const char *name) {
    if (!name || !*name) {
        return NULL;
    }
    for (int i = 0; i < g_flags_count; i++) {
        if (SDL_strcasecmp(g_flags[i].name, name) == 0) {
            return &g_flags[i];
        }
    }
    return NULL;
}

int
sc_cond_flag_count(void) {
    return g_flags_count;
}

const struct sc_cond_flag_def *
sc_cond_flag_at(int index) {
    if (index < 0 || index >= g_flags_count) {
        return NULL;
    }
    return &g_flags[index];
}

bool
sc_cond_flag_registry_ready(void) {
    return g_flags_count > 0;
}

void
sc_cond_flag_set_ctx(void *ctx) {
    g_flags_ctx = ctx;
}

void *
sc_cond_flag_ctx(void) {
    return g_flags_ctx;
}

// ============================================================
// Helpers
// ============================================================

#define COND_ERR_MSG(...)                                \
    do {                                                 \
        if (err && errlen) {                             \
            snprintf(err, errlen, __VA_ARGS__);          \
        }                                                \
    } while (0)

// Copy a trimmed copy of src into dst (dst is at least 64 bytes) and return
// the length of the trimmed text.
static size_t
trim_copy(const char *src, char *dst, size_t dstlen) {
    if (!src) {
        if (dstlen) {
            dst[0] = '\0';
        }
        return 0;
    }
    while (*src && isspace((unsigned char) *src)) {
        src++;
    }
    size_t len = strlen(src);
    while (len > 0 && isspace((unsigned char) src[len - 1])) {
        len--;
    }
    if (len >= dstlen) {
        len = dstlen - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
    return len;
}

// Case insensitive comparison of a prefix; returns the length of the prefix
// when it matches, 0 otherwise.
static size_t
match_prefix(const char *text, const char *prefix) {
    size_t n = strlen(prefix);
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char) text[i]) != tolower((unsigned char) prefix[i])) {
            return 0;
        }
    }
    return n;
}

// ============================================================
// Key expression grammar
// ============================================================

// Strict grammar: a single key, or "A+B[+C[+D]]" (2..4 members, no empty member,
// no duplicate member, no member longer than 63 bytes).
bool
sc_cond_parse_key_expr(const char *name, SDL_Keycode *out, int *nkeys_out,
                       char *err, size_t errlen) {
    if (!name || !*name) {
        COND_ERR_MSG("empty key name");
        return false;
    }

    int nmem = 1;
    for (const char *p = name; *p; p++) {
        if (*p == '+') nmem++;
    }
    if (nmem > SC_COND_MAX_KEYS) {
        COND_ERR_MSG("too many combo members (%d > %d) in '%s'", nmem,
                     SC_COND_MAX_KEYS, name);
        return false;
    }

    if (nmem == 1) {
        SDL_Keycode kc = sc_mapping_parse_key_name(name);
        if (kc == SDLK_UNKNOWN) {
            COND_ERR_MSG("unknown key '%s'", name);
            return false;
        }
        out[0] = kc;
        *nkeys_out = 1;
        return true;
    }

    const char *p = name;
    int n = 0;
    while (true) {
        const char *q = strchr(p, '+');
        size_t len = q ? (size_t) (q - p) : strlen(p);
        if (len == 0) {
            COND_ERR_MSG("empty combo member in '%s'", name);
            return false;
        }
        if (len >= 64) {
            COND_ERR_MSG("combo member too long in '%s'", name);
            return false;
        }
        char tok[64];
        memcpy(tok, p, len);
        tok[len] = '\0';
        // trim
        char *ts = tok;
        while (*ts && isspace((unsigned char) *ts)) ts++;
        char *te = ts + strlen(ts);
        while (te > ts && isspace((unsigned char) *(te - 1))) te--;
        *te = '\0';
        if (*ts == '\0') {
            COND_ERR_MSG("empty combo member in '%s'", name);
            return false;
        }

        SDL_Keycode kc = sc_mapping_parse_key_name(ts);
        if (kc == SDLK_UNKNOWN) {
            COND_ERR_MSG("unknown key '%s' in '%s'", ts, name);
            return false;
        }
        // Mouse buttons have no keyboard press state, so they cannot take part
        // in a combo: a combo requires every member to be held at once.
        if (SC_MAPPING_KEYCODE_IS_MOUSE(kc)) {
            COND_ERR_MSG("mouse button '%s' cannot be a combo member in '%s'",
                         ts, name);
            return false;
        }
        for (int i = 0; i < n; i++) {
            if (out[i] == kc) {
                COND_ERR_MSG("duplicate combo member '%s' in '%s'", ts, name);
                return false;
            }
        }
        out[n++] = kc;
        if (!q) break;
        p = q + 1;
    }

    if (n < 2) {
        COND_ERR_MSG("combo needs at least 2 members: '%s'", name);
        return false;
    }
    *nkeys_out = n;
    return true;
}

// ============================================================
// Dependency reference grammar
// ============================================================

bool
sc_cond_parse_ref(const char *expr, struct sc_mapping_cond *out, char *err,
                  size_t errlen) {
    if (!expr || !*expr) {
        COND_ERR_MSG("empty dependency");
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->state = SC_COND_STATE_ON;

    char text[128];
    trim_copy(expr, text, sizeof(text));
    if (!text[0]) {
        COND_ERR_MSG("empty dependency");
        return false;
    }

    // ---- "entry:<name>" ----
    size_t plen = match_prefix(text, "entry:");
    if (plen) {
        char name[SC_COND_NAME_MAX];
        if (!trim_copy(text + plen, name, sizeof(name)) || !name[0]) {
            COND_ERR_MSG("'entry:' requires a source name");
            return false;
        }
        out->kind = SC_COND_KIND_ENTRY;
        snprintf(out->name, sizeof(out->name), "%s", name);
        return true;
    }

    // ---- "flag:<name>" ----
    plen = match_prefix(text, "flag:");
    if (plen) {
        char name[SC_COND_NAME_MAX];
        if (!trim_copy(text + plen, name, sizeof(name)) || !name[0]) {
            COND_ERR_MSG("'flag:' requires a feature name");
            return false;
        }
        // Only validate the name when the registry is up: the parser must
        // stay usable (and testable) without the application flags.
        if (sc_cond_flag_registry_ready() && !sc_cond_flag_find(name)) {
            COND_ERR_MSG("unknown feature flag '%s'", name);
            return false;
        }
        out->kind = SC_COND_KIND_FLAG;
        snprintf(out->name, sizeof(out->name), "%s", name);
        return true;
    }

    // ---- "key:<expr>" ----
    plen = match_prefix(text, "key:");
    if (plen) {
        char sub[128];
        if (!trim_copy(text + plen, sub, sizeof(sub)) || !sub[0]) {
            COND_ERR_MSG("'key:' requires a key name");
            return false;
        }
        out->kind = SC_COND_KIND_KEY;
        if (!sc_cond_parse_key_expr(sub, out->keys, &out->nkeys, err, errlen)) {
            return false;
        }
        return true;
    }

    // ---- implicit key expression ----
    out->kind = SC_COND_KIND_KEY;
    if (!sc_cond_parse_key_expr(text, out->keys, &out->nkeys, err, errlen)) {
        return false;
    }
    return true;
}

// ============================================================
// Dependency state
// ============================================================

bool
sc_cond_parse_state(const char *text, enum sc_mapping_cond_state *out, char *err,
                    size_t errlen) {
    if (!text || !*text) {
        COND_ERR_MSG("empty depends_state");
        return false;
    }
    if (SDL_strcasecmp(text, "pressed") == 0
            || SDL_strcasecmp(text, "down") == 0
            || SDL_strcasecmp(text, "held") == 0
            || SDL_strcasecmp(text, "active") == 0
            || SDL_strcasecmp(text, "on") == 0
            || SDL_strcasecmp(text, "enabled") == 0
            || SDL_strcasecmp(text, "true") == 0) {
        *out = SC_COND_STATE_ON;
        return true;
    }
    if (SDL_strcasecmp(text, "released") == 0
            || SDL_strcasecmp(text, "up") == 0
            || SDL_strcasecmp(text, "inactive") == 0
            || SDL_strcasecmp(text, "off") == 0
            || SDL_strcasecmp(text, "disabled") == 0
            || SDL_strcasecmp(text, "false") == 0) {
        *out = SC_COND_STATE_OFF;
        return true;
    }
    COND_ERR_MSG("unknown depends_state '%s'", text);
    return false;
}

const char *
sc_cond_state_name(const struct sc_mapping_cond *c) {
    if (!c) {
        return "pressed";
    }
    bool on = c->state == SC_COND_STATE_ON;
    switch (c->kind) {
        case SC_COND_KIND_KEY:
            return on ? "pressed" : "released";
        case SC_COND_KIND_ENTRY:
            return on ? "active" : "inactive";
        case SC_COND_KIND_FLAG:
        default:
            return on ? "on" : "off";
    }
}

const char *
sc_cond_to_name(const struct sc_mapping_cond *c, char *buf, size_t buflen) {
    if (!buf || buflen == 0) {
        return NULL;
    }
    buf[0] = '\0';
    if (!c) {
        return buf;
    }
    switch (c->kind) {
        case SC_COND_KIND_ENTRY:
            snprintf(buf, buflen, "entry:%s", c->name);
            return buf;
        case SC_COND_KIND_FLAG:
            snprintf(buf, buflen, "flag:%s", c->name);
            return buf;
        case SC_COND_KIND_KEY:
        default:
            break;
    }
    size_t off = 0;
    for (int i = 0; i < c->nkeys && off < buflen - 1; i++) {
        const char *n = sc_mapping_key_name(c->keys[i]);
        if (i > 0 && off < buflen - 1) {
            buf[off++] = '+';
        }
        while (*n && off < buflen - 1) {
            buf[off++] = *n++;
        }
    }
    buf[off] = '\0';
    return buf;
}

void
sc_cond_init_key(struct sc_mapping_cond *c, SDL_Keycode kc,
                 enum sc_mapping_cond_state state) {
    if (!c) {
        return;
    }
    memset(c, 0, sizeof(*c));
    c->kind = SC_COND_KIND_KEY;
    c->state = state;
    c->keys[0] = kc;
    c->nkeys = 1;
}
