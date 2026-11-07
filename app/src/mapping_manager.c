#include "mapping_manager.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <SDL3/SDL.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

#include "util/cJSON.h"
#include "util/log.h"

#define SWITCH_DEBOUNCE_MS 300
#define SLIDE_DURATION_DEFAULT_MS 200
#define SLIDE_DURATION_MIN_MS 50
#define SLIDE_DURATION_MAX_MS 5000

// Reserved mouse entry names ("keys" without a keycode)
static bool
is_mouse_right(const char *name);

static bool
is_mouse_left_locked(const char *name);

static bool
is_mouse_name(const char *name) {
    return is_mouse_right(name) || is_mouse_left_locked(name);
}

// ============================================================
// Path / file helpers
// ============================================================

char *
sc_mapping_get_base_dir(void) {
    const char *base = SDL_GetBasePath();
    if (base) {
        char *copy = SDL_strdup(base);
        SDL_free((void *) base);
        return copy;
    }
    return SDL_strdup("./");
}

static char *
join_path(const char *dir, const char *name) {
    size_t dlen = strlen(dir);
    size_t nlen = strlen(name);
    bool need_sep = (dlen > 0) && (dir[dlen - 1] != '/' && dir[dlen - 1] != '\\');
    char *out = malloc(dlen + nlen + (need_sep ? 2 : 1));
    if (!out) return NULL;
    memcpy(out, dir, dlen);
    if (need_sep) out[dlen++] = '/';
    memcpy(out + dlen, name, nlen + 1);
    return out;
}

// Build ".../templates/<name>.json". Caller must free the result.
static char *
template_path(const struct sc_mapping_manager *mm, const char *name) {
    if (!mm->templates_dir || !name || !*name) return NULL;
    char *base = join_path(mm->templates_dir, name);
    if (!base) return NULL;
    size_t len = strlen(base);
    char *full = malloc(len + 6);
    if (!full) {
        free(base);
        return NULL;
    }
    snprintf(full, len + 6, "%s.json", base);
    free(base);
    return full;
}

// Sub-second-aware stamp: mtime (nanoseconds) + size. Any single edit changes
// the size or the mtime with at least 100 ns precision on Windows.
static struct sc_file_stamp
file_stamp(const char *path) {
    struct sc_file_stamp s = { 0 };
    if (!path) return s;
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) {
        return s;
    }
    int64_t ft = ((int64_t) fad.ftLastWriteTime.dwHighDateTime << 32)
               | (int64_t) (uint32_t) fad.ftLastWriteTime.dwLowDateTime;
    s.mtime_ns = ft * 100; // 100 ns units
    s.size = ((int64_t) fad.nFileSizeHigh << 32)
           | (int64_t) (uint32_t) fad.nFileSizeLow;
    s.valid = true;
#else
    struct stat st;
    if (stat(path, &st) != 0) {
        return s;
    }
#if defined(__APPLE__)
    s.mtime_ns = (int64_t) st.st_mtimespec.tv_sec * 1000000000LL
               + st.st_mtimespec.tv_nsec;
#else
    s.mtime_ns = (int64_t) st.st_mtim.tv_sec * 1000000000LL
               + st.st_mtim.tv_nsec;
#endif
    s.size = (int64_t) st.st_size;
    s.valid = true;
#endif
    return s;
}

static bool
stamp_equal(const struct sc_file_stamp *a, const struct sc_file_stamp *b) {
    if (a->valid != b->valid) return false;
    if (!a->valid) return true; // both missing
    return a->mtime_ns == b->mtime_ns && a->size == b->size;
}

static char *
read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t) sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t) sz, f);
    fclose(f);
    buf[rd] = '\0';
    return buf;
}

// Temporary file + atomic replace, so a reader never sees a half-written file.
static bool
write_file_atomic(const char *path, const char *text) {
    size_t len = strlen(path);
    char *tmp = malloc(len + 8);
    if (!tmp) return false;
    snprintf(tmp, len + 8, "%s.tmp", path);

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        LOGW("Could not open for writing: %s", tmp);
        free(tmp);
        return false;
    }
    bool ok = fputs(text, f) != EOF;
    fflush(f);
    fclose(f);
    if (!ok) {
        LOGW("Could not write: %s", tmp);
        remove(tmp);
        free(tmp);
        return false;
    }

#ifdef _WIN32
    ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) {
        LOGW("Could not replace '%s': error %lu", path, GetLastError());
        remove(tmp);
    }
#else
    ok = rename(tmp, path) == 0;
    if (!ok) {
        LOGW("Could not replace '%s': %s", path, strerror(errno));
        remove(tmp);
    }
#endif
    free(tmp);
    return ok;
}

// ============================================================
// Key name <-> keycode
// ============================================================

SDL_Keycode
sc_mapping_parse_key_name(const char *name) {
    if (!name) return SDLK_UNKNOWN;
    char buf[64];
    strncpy(buf, name, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *start = buf;
    while (*start && isspace((unsigned char) *start)) start++;
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char) *(end - 1))) end--;
    *end = '\0';
    if (*start == '\0') return SDLK_UNKNOWN;

    if (SDL_strcasecmp(start, "space") == 0) return SDLK_SPACE;
    if (SDL_strcasecmp(start, "lshift") == 0 ||
        SDL_strcasecmp(start, "left shift") == 0 ||
        SDL_strcasecmp(start, "shift") == 0) return SDLK_LSHIFT;
    if (SDL_strcasecmp(start, "rshift") == 0 ||
        SDL_strcasecmp(start, "right shift") == 0) return SDLK_RSHIFT;
    if (SDL_strcasecmp(start, "lctrl") == 0 ||
        SDL_strcasecmp(start, "left ctrl") == 0 ||
        SDL_strcasecmp(start, "ctrl") == 0) return SDLK_LCTRL;
    if (SDL_strcasecmp(start, "rctrl") == 0 ||
        SDL_strcasecmp(start, "right ctrl") == 0) return SDLK_RCTRL;
    if (SDL_strcasecmp(start, "lalt") == 0 ||
        SDL_strcasecmp(start, "left alt") == 0 ||
        SDL_strcasecmp(start, "alt") == 0) return SDLK_LALT;
    if (SDL_strcasecmp(start, "ralt") == 0 ||
        SDL_strcasecmp(start, "right alt") == 0) return SDLK_RALT;
    if (SDL_strcasecmp(start, "esc") == 0 ||
        SDL_strcasecmp(start, "escape") == 0) return SDLK_ESCAPE;
    if (SDL_strcasecmp(start, "enter") == 0 ||
        SDL_strcasecmp(start, "return") == 0) return SDLK_RETURN;
    if (SDL_strcasecmp(start, "tab") == 0) return SDLK_TAB;
    if (SDL_strcasecmp(start, "backspace") == 0) return SDLK_BACKSPACE;

    if (strlen(start) == 1) {
        char c = (char) toupper((unsigned char) start[0]);
        if (c >= 'A' && c <= 'Z') return SDLK_A + (c - 'A');
        if (c >= '0' && c <= '9') return SDLK_0 + (c - '0');
    }

    SDL_Keycode kc = SDL_GetKeyFromName(start);
    if (kc != SDLK_UNKNOWN) return kc;

    char upper[64];
    size_t n = strlen(start);
    for (size_t i = 0; i < n && i < sizeof(upper) - 1; i++) {
        upper[i] = (char) toupper((unsigned char) start[i]);
    }
    upper[n < sizeof(upper) ? n : sizeof(upper) - 1] = '\0';
    kc = SDL_GetKeyFromName(upper);
    if (kc != SDLK_UNKNOWN) return kc;

    return SDLK_UNKNOWN;
}

const char *
sc_mapping_key_name(SDL_Keycode kc) {
    const char *n = SDL_GetKeyName(kc);
    if (n && *n) return n;
    return "Unknown";
}

// ============================================================
// Key expression grammar
// ============================================================

#define ERR_MSG(...)                                  \
    do {                                             \
        if (err && errlen) {                         \
            snprintf(err, errlen, __VA_ARGS__);      \
        }                                            \
    } while (0)

// Strict grammar: a single key, or "A+B[+C[+D]]" (2..4 members, no empty member,
// no duplicate member, no member longer than 63 bytes).
static bool
parse_key_expr(const char *name, SDL_Keycode *out, int *nkeys_out,
               char *err, size_t errlen) {
    if (!name || !*name) {
        ERR_MSG("empty key name");
        return false;
    }

    int nmem = 1;
    for (const char *p = name; *p; p++) {
        if (*p == '+') nmem++;
    }
    if (nmem > SC_MAPPING_MAX_KEYS) {
        ERR_MSG("too many combo members (%d > %d)", nmem, SC_MAPPING_MAX_KEYS);
        return false;
    }

    if (nmem == 1) {
        SDL_Keycode kc = sc_mapping_parse_key_name(name);
        if (kc == SDLK_UNKNOWN) {
            ERR_MSG("unknown key '%s'", name);
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
            ERR_MSG("empty combo member in '%s'", name);
            return false;
        }
        if (len >= 64) {
            ERR_MSG("combo member too long in '%s'", name);
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
            ERR_MSG("empty combo member in '%s'", name);
            return false;
        }

        SDL_Keycode kc = sc_mapping_parse_key_name(ts);
        if (kc == SDLK_UNKNOWN) {
            ERR_MSG("unknown key '%s' in '%s'", ts, name);
            return false;
        }
        for (int i = 0; i < n; i++) {
            if (out[i] == kc) {
                ERR_MSG("duplicate combo member '%s' in '%s'", ts, name);
                return false;
            }
        }
        out[n++] = kc;
        if (!q) break;
        p = q + 1;
    }

    if (n < 2) {
        ERR_MSG("combo needs at least 2 members: '%s'", name);
        return false;
    }
    *nkeys_out = n;
    return true;
}

bool
sc_mapping_validate_key_expr(const char *name, char *err, size_t errlen) {
    // Reserved mouse names are valid "keys" (they carry no keycode).
    if (is_mouse_name(name)) {
        return true;
    }
    SDL_Keycode keys[SC_MAPPING_MAX_KEYS];
    int nkeys = 0;
    return parse_key_expr(name, keys, &nkeys, err, errlen);
}

// ============================================================
// Special mouse names
// ============================================================

static bool
is_mouse_right(const char *name) {
    return name && (SDL_strcasecmp(name, "MOUSE_RIGHT") == 0
                 || SDL_strcasecmp(name, "RIGHT_CLICK") == 0);
}

static bool
is_mouse_left_locked(const char *name) {
    return name && (SDL_strcasecmp(name, "MOUSE_LEFT_LOCKED") == 0
                 || SDL_strcasecmp(name, "LEFT_CLICK_LOCKED") == 0);
}

// ============================================================
// Pointer id allocator
// ============================================================

// Values -1, -2 and -3 are reserved by scrcpy (generic finger, mouse, virtual
// finger), so ids are allocated in [-4, -(MAX_ENTRIES+3)] through an occupancy
// table: two entries alive at the same time never share an id, and an id is
// released only after the entry is removed (after its ACTION_UP was injected).
static int32_t
acquire_pid(struct sc_mapping_manager *mm) {
    for (int i = 0; i < SC_MAPPING_MAX_ENTRIES; i++) {
        int slot = mm->pid_cursor;
        mm->pid_cursor = (mm->pid_cursor + 1) % SC_MAPPING_MAX_ENTRIES;
        if (!mm->pid_used[slot]) {
            mm->pid_used[slot] = true;
            return -(SC_MAPPING_PID_MIN + slot);
        }
    }
    return 0;
}

static void
release_pid(struct sc_mapping_manager *mm, int32_t pid) {
    if (pid >= 0) return;
    int slot = -pid - SC_MAPPING_PID_MIN;
    if (slot >= 0 && slot < SC_MAPPING_MAX_ENTRIES) {
        mm->pid_used[slot] = false;
    }
}

// ============================================================
// Entry table helpers
// ============================================================

static void
entry_clear(struct sc_mapping_entry *e) {
    free(e->key_name);
    memset(e, 0, sizeof(*e));
}

static void
free_all_entries(struct sc_mapping_manager *mm) {
    for (int i = 0; i < SC_MAPPING_MAX_ENTRIES; i++) {
        entry_clear(&mm->entries[i]);
    }
    mm->count = 0;
}

static void
refresh_mouse_indices(struct sc_mapping_manager *mm) {
    mm->mouse_right_idx = -1;
    mm->mouse_left_locked_idx = -1;
    for (int i = 0; i < SC_MAPPING_MAX_ENTRIES; i++) {
        if (!mm->entries[i].key_name) continue;
        if (is_mouse_right(mm->entries[i].key_name)) {
            mm->mouse_right_idx = i;
        } else if (is_mouse_left_locked(mm->entries[i].key_name)) {
            mm->mouse_left_locked_idx = i;
        }
    }
}

static int
find_free_slot(struct sc_mapping_manager *mm) {
    for (int i = 0; i < SC_MAPPING_MAX_ENTRIES; i++) {
        if (!mm->entries[i].key_name) return i;
    }
    return -1;
}

static void
recount(struct sc_mapping_manager *mm) {
    int n = 0;
    for (int i = 0; i < SC_MAPPING_MAX_ENTRIES; i++) {
        if (mm->entries[i].key_name) n++;
    }
    mm->count = n;
}

static struct sc_mapping_entry *
find_by_name_impl(struct sc_mapping_manager *mm, const char *key_name) {
    if (!key_name) return NULL;
    for (int i = 0; i < SC_MAPPING_MAX_ENTRIES; i++) {
        struct sc_mapping_entry *e = &mm->entries[i];
        if (e->key_name && SDL_strcasecmp(e->key_name, key_name) == 0) {
            return e;
        }
    }
    return NULL;
}

// ============================================================
// Template parsing
// ============================================================

struct sc_parse_out {
    struct sc_mapping_entry *entries; // SC_MAPPING_MAX_ENTRIES slots
    int count;
    struct sc_mapping_load_stats stats;
};

static void
stat_fail(struct sc_mapping_load_stats *st, const char *tmpl, int index,
          const char *reason) {
    st->fail++;
    if (st->nmsgs < SC_MAPPING_MAX_FAILS) {
        snprintf(st->msgs[st->nmsgs], sizeof(st->msgs[0]),
                 "template '%s' entry #%d rejected: %s", tmpl, index, reason);
        st->nmsgs++;
    }
    LOGW("Template '%s' entry #%d rejected: %s", tmpl, index, reason);
}

static void
parse_clamp_duration(int *duration_ms, const char *tmpl, int index) {
    if (*duration_ms < SLIDE_DURATION_MIN_MS) {
        LOGW("Template '%s' entry #%d: duration_ms %d < %d, clamped",
             tmpl, index, *duration_ms, SLIDE_DURATION_MIN_MS);
        *duration_ms = SLIDE_DURATION_MIN_MS;
    } else if (*duration_ms > SLIDE_DURATION_MAX_MS) {
        LOGW("Template '%s' entry #%d: duration_ms %d > %d, clamped",
             tmpl, index, *duration_ms, SLIDE_DURATION_MAX_MS);
        *duration_ms = SLIDE_DURATION_MAX_MS;
    }
}

static void
cross_validate_slides(struct sc_parse_out *out, const char *path);

static bool
parse_template(const char *path, struct sc_parse_out *out) {
    memset(out, 0, sizeof(*out));
    out->entries = calloc(SC_MAPPING_MAX_ENTRIES, sizeof(*out->entries));
    if (!out->entries) return false;

    char *text = read_file(path);
    if (!text) {
        LOGE("Could not open template: %s", path);
        return false;
    }
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        LOGE("Invalid JSON in template: %s", path);
        return false;
    }
    if (!cJSON_IsObject(root)) {
        LOGE("Template root is not an object: %s", path);
        cJSON_Delete(root);
        return false;
    }

    cJSON *mappings = cJSON_GetObjectItemCaseSensitive(root, "mappings");
    if (!cJSON_IsArray(mappings)) {
        LOGE("Template has no 'mappings' array: %s", path);
        cJSON_Delete(root);
        return false;
    }

    int n = cJSON_GetArraySize(mappings);
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(mappings, i);
        char err[128];

        if (!cJSON_IsObject(item)) {
            stat_fail(&out->stats, path, i, "entry is not an object");
            continue;
        }

        cJSON *jk = cJSON_GetObjectItemCaseSensitive(item, "key");
        if (!cJSON_IsString(jk) || !jk->valuestring || !*jk->valuestring) {
            stat_fail(&out->stats, path, i, "missing or invalid 'key'");
            continue;
        }
        const char *key_name = jk->valuestring;

        // duplicate key (case-insensitive)
        bool dup = false;
        for (int k = 0; k < out->count; k++) {
            if (SDL_strcasecmp(out->entries[k].key_name, key_name) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            snprintf(err, sizeof(err), "duplicate key '%s'", key_name);
            stat_fail(&out->stats, path, i, err);
            continue;
        }

        cJSON *jx = cJSON_GetObjectItemCaseSensitive(item, "x");
        cJSON *jy = cJSON_GetObjectItemCaseSensitive(item, "y");
        if (!cJSON_IsNumber(jx) || !cJSON_IsNumber(jy)) {
            stat_fail(&out->stats, path, i, "missing or invalid 'x'/'y'");
            continue;
        }
        double nx = jx->valuedouble;
        double ny = jy->valuedouble;
        if (nx < 0.0 || nx > 1.0 || ny < 0.0 || ny > 1.0) {
            snprintf(err, sizeof(err), "'x'/'y' out of range [0,1] (%f, %f)",
                     nx, ny);
            stat_fail(&out->stats, path, i, err);
            continue;
        }

        // type (optional, defaults to "touch")
        enum sc_mapping_entry_type type = SC_MAPPING_TYPE_TOUCH;
        cJSON *jt = cJSON_GetObjectItemCaseSensitive(item, "type");
        if (jt && !cJSON_IsString(jt)) {
            stat_fail(&out->stats, path, i, "'type' must be a string");
            continue;
        }
        if (cJSON_IsString(jt)) {
            if (SDL_strcasecmp(jt->valuestring, "touch") == 0) {
                type = SC_MAPPING_TYPE_TOUCH;
            } else if (SDL_strcasecmp(jt->valuestring, "slide") == 0) {
                type = SC_MAPPING_TYPE_SLIDE;
            } else {
                snprintf(err, sizeof(err), "unknown type '%s'",
                         jt->valuestring);
                stat_fail(&out->stats, path, i, err);
                continue;
            }
        }

        // mouse reserved names
        if (is_mouse_name(key_name)) {
            if (type == SC_MAPPING_TYPE_SLIDE) {
                snprintf(err, sizeof(err),
                         "'%s' cannot be a slide", key_name);
                stat_fail(&out->stats, path, i, err);
                continue;
            }
            struct sc_mapping_entry *e = &out->entries[out->count];
            e->key_name = strdup(key_name);
            if (!e->key_name) {
                stat_fail(&out->stats, path, i, "out of memory");
                continue;
            }
            e->nkeys = 0;
            e->is_mouse = true;
            e->type = SC_MAPPING_TYPE_TOUCH;
            e->nx = nx;
            e->ny = ny;
            e->duration_ms = 0;
            out->count++;
            out->stats.ok++;
            continue;
        }

        SDL_Keycode keys[SC_MAPPING_MAX_KEYS];
        int nkeys = 0;
        if (!parse_key_expr(key_name, keys, &nkeys, err, sizeof(err))) {
            stat_fail(&out->stats, path, i, err);
            continue;
        }

        if (type == SC_MAPPING_TYPE_SLIDE) {
            if (nkeys != 1) {
                snprintf(err, sizeof(err),
                         "slide key must be a single key, not a combo ('%s')",
                         key_name);
                stat_fail(&out->stats, path, i, err);
                continue;
            }
            cJSON *jdx = cJSON_GetObjectItemCaseSensitive(item, "dx");
            cJSON *jdy = cJSON_GetObjectItemCaseSensitive(item, "dy");
            if (!cJSON_IsNumber(jdx) || !cJSON_IsNumber(jdy)) {
                stat_fail(&out->stats, path, i,
                          "slide requires numeric 'dx' and 'dy'");
                continue;
            }
            double ndx = jdx->valuedouble;
            double ndy = jdy->valuedouble;
            if (ndx < -1.0 || ndx > 1.0 || ndy < -1.0 || ndy > 1.0) {
                snprintf(err, sizeof(err),
                         "'dx'/'dy' out of range [-1,1] (%f, %f)", ndx, ndy);
                stat_fail(&out->stats, path, i, err);
                continue;
            }
            int duration_ms = SLIDE_DURATION_DEFAULT_MS;
            cJSON *jdur = cJSON_GetObjectItemCaseSensitive(item, "duration_ms");
            if (jdur) {
                if (!cJSON_IsNumber(jdur)) {
                    stat_fail(&out->stats, path, i,
                              "'duration_ms' must be a number");
                    continue;
                }
                duration_ms = (int) jdur->valuedouble;
            }
            parse_clamp_duration(&duration_ms, path, i);

            struct sc_mapping_entry *e = &out->entries[out->count];
            e->key_name = strdup(key_name);
            if (!e->key_name) {
                stat_fail(&out->stats, path, i, "out of memory");
                continue;
            }
            memcpy(e->keys, keys, sizeof(keys));
            e->nkeys = nkeys;
            e->type = SC_MAPPING_TYPE_SLIDE;
            e->nx = nx;
            e->ny = ny;
            e->ndx = ndx;
            e->ndy = ndy;
            e->duration_ms = duration_ms;
            out->count++;
            out->stats.ok++;
            continue;
        }

        struct sc_mapping_entry *e = &out->entries[out->count];
        e->key_name = strdup(key_name);
        if (!e->key_name) {
            stat_fail(&out->stats, path, i, "out of memory");
            continue;
        }
        memcpy(e->keys, keys, sizeof(keys));
        e->nkeys = nkeys;
        e->type = SC_MAPPING_TYPE_TOUCH;
        e->nx = nx;
        e->ny = ny;
        e->duration_ms = 0;
        out->count++;
        out->stats.ok++;
    }

    if (out->count >= SC_MAPPING_MAX_ENTRIES) {
        LOGW("Template '%s': entry table full, extra entries ignored", path);
    }

    cross_validate_slides(out, path);

    cJSON_Delete(root);
    return true;
}

static void
parse_out_free(struct sc_parse_out *out) {
    if (!out->entries) return;
    for (int i = 0; i < out->count; i++) {
        free(out->entries[i].key_name);
    }
    free(out->entries);
    out->entries = NULL;
    out->count = 0;
}

// A slide key must not also take part in a combo (the combo would press it as a
// plain tap while the slide owns the same pointer id).
static void
cross_validate_slides(struct sc_parse_out *out, const char *path) {
    for (int i = 0; i < out->count; i++) {
        struct sc_mapping_entry *e = &out->entries[i];
        if (!e->key_name || e->type != SC_MAPPING_TYPE_SLIDE) continue;
        bool reject = false;
        for (int j = 0; j < out->count && !reject; j++) {
            struct sc_mapping_entry *c = &out->entries[j];
            if (j == i || !c->key_name || c->nkeys < 2) continue;
            for (int k = 0; k < c->nkeys; k++) {
                if (c->keys[k] == e->keys[0]) {
                    reject = true;
                    break;
                }
            }
        }
        if (reject) {
            char reason[128];
            snprintf(reason, sizeof(reason),
                     "slide key '%s' is also used in a combo", e->key_name);
            stat_fail(&out->stats, path, i, reason);
            free(e->key_name);
            e->key_name = NULL;
        }
    }
    // compact
    int n = 0;
    for (int i = 0; i < out->count; i++) {
        if (!out->entries[i].key_name) continue;
        if (n != i) out->entries[n] = out->entries[i];
        n++;
    }
    out->count = n;
}

// Atomically replace the live entries by the parsed ones. The release hook is
// called first so that no finger stays pressed on the device.
static void
commit_entries(struct sc_mapping_manager *mm, struct sc_parse_out *out) {
    if (mm->release_hook) {
        mm->release_hook(mm->release_opaque);
    }
    free_all_entries(mm);
    memset(mm->pid_used, 0, sizeof(mm->pid_used));
    mm->pid_cursor = 0;

    for (int i = 0; i < out->count && i < SC_MAPPING_MAX_ENTRIES; i++) {
        mm->entries[i] = out->entries[i]; // ownership transfer
        mm->entries[i].pointer_id = acquire_pid(mm);
        mm->entries[i].down = false;
        mm->entries[i].active = false;
        out->entries[i].key_name = NULL; // transferred
    }
    mm->count = out->count;
    refresh_mouse_indices(mm);
}

// ============================================================
// Saving
// ============================================================

static bool
save_active_impl(struct sc_mapping_manager *mm, bool force) {
    if (!mm->active_name) return false;
    if (!force && !mm->dirty) {
        return true; // nothing to do: avoid useless disk writes
    }
    char *full = template_path(mm, mm->active_name);
    if (!full) return false;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", mm->active_name);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < SC_MAPPING_MAX_ENTRIES; i++) {
        struct sc_mapping_entry *e = &mm->entries[i];
        if (!e->key_name) continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "key", e->key_name);
        cJSON_AddNumberToObject(item, "x", e->nx);
        cJSON_AddNumberToObject(item, "y", e->ny);
        if (e->type == SC_MAPPING_TYPE_SLIDE) {
            cJSON_AddStringToObject(item, "type", "slide");
            cJSON_AddNumberToObject(item, "dx", e->ndx);
            cJSON_AddNumberToObject(item, "dy", e->ndy);
            cJSON_AddNumberToObject(item, "duration_ms", e->duration_ms);
        }
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(root, "mappings", arr);

    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text) { free(full); return false; }

    bool ok = write_file_atomic(full, text);
    cJSON_free(text);
    if (ok) {
        mm->active_stamp = file_stamp(full);
        mm->dirty = false;
        LOGI("Saved template '%s'", mm->active_name);
    } else {
        LOGE("Could not save template '%s'", mm->active_name);
    }
    free(full);
    return ok;
}

bool
sc_mapping_manager_save_active(struct sc_mapping_manager *mm) {
    return save_active_impl(mm, false);
}

bool
sc_mapping_manager_save_active_force(struct sc_mapping_manager *mm) {
    return save_active_impl(mm, true);
}

// ============================================================
// Config
// ============================================================

static void
load_config(struct sc_mapping_manager *mm) {
    char *text = read_file(mm->config_path);
    if (!text) {
        LOGI("No config file found, using defaults");
        mm->config_stamp = file_stamp(mm->config_path);
        return;
    }
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        LOGW("Invalid config JSON: %s", mm->config_path);
        return;
    }

    cJSON *shortcuts = cJSON_GetObjectItemCaseSensitive(root, "shortcuts");
    if (cJSON_IsObject(shortcuts)) {
        static const char *names[4] = {
            "switch_template", "design_mode", "toggle_mapping", "manager_panel",
        };
        SDL_Keycode *slots[4] = {
            &mm->sc_switch, &mm->sc_design, &mm->sc_toggle, &mm->sc_manager,
        };
        for (int i = 0; i < 4; i++) {
            cJSON *it = cJSON_GetObjectItemCaseSensitive(shortcuts, names[i]);
            if (cJSON_IsString(it)) {
                SDL_Keycode kc = sc_mapping_parse_key_name(it->valuestring);
                if (kc != SDLK_UNKNOWN) *slots[i] = kc;
            }
        }
    }

    cJSON *active = cJSON_GetObjectItemCaseSensitive(root, "active_template");
    if (cJSON_IsString(active) && *active->valuestring) {
        free(mm->active_name);
        mm->active_name = strdup(active->valuestring);
    }

    cJSON *en = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    if (cJSON_IsBool(en)) {
        mm->enabled = cJSON_IsTrue(en);
    }

    cJSON_Delete(root);
    mm->config_stamp = file_stamp(mm->config_path);
}

static bool
save_config(struct sc_mapping_manager *mm) {
    cJSON *root = cJSON_CreateObject();
    cJSON *shortcuts = cJSON_CreateObject();
    cJSON_AddStringToObject(shortcuts, "switch_template",
                            sc_mapping_key_name(mm->sc_switch));
    cJSON_AddStringToObject(shortcuts, "design_mode",
                            sc_mapping_key_name(mm->sc_design));
    cJSON_AddStringToObject(shortcuts, "toggle_mapping",
                            sc_mapping_key_name(mm->sc_toggle));
    cJSON_AddStringToObject(shortcuts, "manager_panel",
                            sc_mapping_key_name(mm->sc_manager));
    cJSON_AddItemToObject(root, "shortcuts", shortcuts);
    if (mm->active_name) {
        cJSON_AddStringToObject(root, "active_template", mm->active_name);
    }
    cJSON_AddBoolToObject(root, "enabled", mm->enabled);

    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text) return false;

    bool ok = write_file_atomic(mm->config_path, text);
    cJSON_free(text);
    if (ok) {
        mm->config_stamp = file_stamp(mm->config_path);
    } else {
        LOGE("Could not save config: %s", mm->config_path);
    }
    return ok;
}

// ============================================================
// Template listing
// ============================================================

int
sc_mapping_manager_list(struct sc_mapping_manager *mm, char **names, int max) {
    int total = 0;

#ifdef _WIN32
    char pattern[512];
    snprintf(pattern, sizeof(pattern), "%s*.json", mm->templates_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const char *fn = fd.cFileName;
        size_t len = strlen(fn);
        if (len < 5 || SDL_strcasecmp(fn + len - 5, ".json") != 0) continue;
        if (total < max && names) {
            names[total] = malloc(len - 4);
            if (names[total]) {
                memcpy(names[total], fn, len - 5);
                names[total][len - 5] = '\0';
            }
        }
        total++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(mm->templates_dir);
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *fn = ent->d_name;
        size_t len = strlen(fn);
        if (len < 5 || SDL_strcasecmp(fn + len - 5, ".json") != 0) continue;
        if (total < max && names) {
            names[total] = malloc(len - 4);
            if (names[total]) {
                memcpy(names[total], fn, len - 5);
                names[total][len - 5] = '\0';
            }
        }
        total++;
    }
    closedir(d);
#endif
    return total;
}

// ============================================================
// Load / switch
// ============================================================

// Load a template into memory. Nothing is changed when parsing fails: the
// current entries stay live (no half-applied state).
static bool
load_template_into_mm(struct sc_mapping_manager *mm, const char *name) {
    char *full = template_path(mm, name);
    if (!full) return false;

    struct sc_parse_out out;
    bool parsed = parse_template(full, &out);
    if (parsed) {
        commit_entries(mm, &out);
        mm->dirty = false;
    }
    mm->last_load = out.stats;
    parse_out_free(&out);

    mm->active_stamp = file_stamp(full);
    free(full);

    if (!parsed) return false;

    LOGI("Loaded template '%s': %d entries, %d rejected", name,
         mm->count, mm->last_load.fail);
    return true;
}

bool
sc_mapping_manager_switch(struct sc_mapping_manager *mm, const char *name) {
    if (!name) return false;
    bool same = mm->active_name && strcmp(mm->active_name, name) == 0;

    char *old = mm->active_name ? strdup(mm->active_name) : NULL;

    // Load first (rolls nothing back on failure: entries are untouched).
    if (!load_template_into_mm(mm, name)) {
        LOGE("Could not load template '%s', keeping '%s'", name,
             old ? old : "(none)");
        free(old);
        return false;
    }

    free(mm->active_name);
    mm->active_name = strdup(name);

    // Persist only when the active template really changed.
    if (!same) {
        save_config(mm);
    }
    free(old);
    return true;
}

void
sc_mapping_manager_request_switch(struct sc_mapping_manager *mm,
                                  const char *name) {
    if (!name || !*name) return;
    if (mm->active_name && strcmp(mm->active_name, name) == 0) {
        free(mm->pending_switch);
        mm->pending_switch = NULL;
        return;
    }
    if (mm->pending_switch && strcmp(mm->pending_switch, name) == 0) {
        return; // already pending: keep the initial timestamp
    }
    free(mm->pending_switch);
    mm->pending_switch = strdup(name);
    mm->pending_since = sc_tick_now();
}

static void
apply_pending_switch(struct sc_mapping_manager *mm) {
    if (!mm->pending_switch) return;
    if (sc_tick_now() - mm->pending_since < SC_TICK_FROM_MS(SWITCH_DEBOUNCE_MS)) {
        return;
    }
    char *name = mm->pending_switch;
    mm->pending_switch = NULL;
    sc_mapping_manager_switch(mm, name);
    free(name);
}

void
sc_mapping_manager_set_enabled(struct sc_mapping_manager *mm, bool enabled) {
    if (mm->enabled == enabled) return;
    if (!enabled && mm->release_hook) {
        // Releasing must happen before the flag flips, so held fingers are
        // lifted while the hook is still allowed to inject.
        mm->release_hook(mm->release_opaque);
    }
    mm->enabled = enabled;
    save_config(mm);
    LOGI("Key mapping %s", enabled ? "ENABLED" : "DISABLED");
}

// ============================================================
// Lookup helpers
// ============================================================

struct sc_mapping_entry *
sc_mapping_manager_find_single(struct sc_mapping_manager *mm, SDL_Keycode kc) {
    for (int i = 0; i < SC_MAPPING_MAX_ENTRIES; i++) {
        struct sc_mapping_entry *e = &mm->entries[i];
        if (e->key_name && e->nkeys == 1 && e->keys[0] == kc) return e;
    }
    return NULL;
}

struct sc_mapping_entry *
sc_mapping_manager_find_by_name(struct sc_mapping_manager *mm,
                                const char *key_name) {
    return find_by_name_impl(mm, key_name);
}

bool
sc_mapping_manager_key_in_combo(struct sc_mapping_manager *mm, SDL_Keycode kc) {
    for (int i = 0; i < SC_MAPPING_MAX_ENTRIES; i++) {
        struct sc_mapping_entry *e = &mm->entries[i];
        if (e->key_name && e->nkeys >= 2) {
            for (int j = 0; j < e->nkeys; j++) {
                if (e->keys[j] == kc) return true;
            }
        }
    }
    return false;
}

int
sc_mapping_manager_entry_count(struct sc_mapping_manager *mm) {
    return mm->count;
}

// ============================================================
// Entry upsert / removal
// ============================================================

bool
sc_mapping_manager_upsert(struct sc_mapping_manager *mm, const char *key_name,
                          enum sc_mapping_entry_type type, double nx, double ny,
                          double ndx, double ndy, int duration_ms,
                          char *err, size_t errlen) {
    if (!key_name || !*key_name) {
        ERR_MSG("empty key name");
        return false;
    }
    if (nx < 0.0 || nx > 1.0 || ny < 0.0 || ny > 1.0) {
        ERR_MSG("'x'/'y' out of range [0,1]");
        return false;
    }

    bool is_mouse = is_mouse_name(key_name);
    SDL_Keycode keys[SC_MAPPING_MAX_KEYS];
    int nkeys = 0;
    if (!is_mouse && !parse_key_expr(key_name, keys, &nkeys, err, errlen)) {
        return false;
    }
    if (is_mouse && type == SC_MAPPING_TYPE_SLIDE) {
        ERR_MSG("mouse entries cannot be slides");
        return false;
    }
    if (type == SC_MAPPING_TYPE_SLIDE) {
        if (nkeys != 1) {
            ERR_MSG("slide key must be a single key");
            return false;
        }
        if (ndx < -1.0 || ndx > 1.0 || ndy < -1.0 || ndy > 1.0) {
            ERR_MSG("'dx'/'dy' out of range [-1,1]");
            return false;
        }
    }
    if (duration_ms == 0) duration_ms = SLIDE_DURATION_DEFAULT_MS;
    if (duration_ms < SLIDE_DURATION_MIN_MS) {
        duration_ms = SLIDE_DURATION_MIN_MS;
    } else if (duration_ms > SLIDE_DURATION_MAX_MS) {
        duration_ms = SLIDE_DURATION_MAX_MS;
    }

    // A slide key must not be used in a combo (and vice versa): the slide owns
    // a pointer id, a combo press would tap with the same id.
    if (!is_mouse) {
        if (type == SC_MAPPING_TYPE_SLIDE) {
            for (int i = 0; i < SC_MAPPING_MAX_ENTRIES; i++) {
                struct sc_mapping_entry *c = &mm->entries[i];
                if (!c->key_name || c->nkeys < 2) continue;
                for (int j = 0; j < c->nkeys; j++) {
                    if (c->keys[j] == keys[0]) {
                        ERR_MSG("key '%s' is already used in combo '%s'",
                                key_name, c->key_name);
                        return false;
                    }
                }
            }
        } else if (nkeys >= 2) {
            for (int i = 0; i < SC_MAPPING_MAX_ENTRIES; i++) {
                struct sc_mapping_entry *s = &mm->entries[i];
                if (!s->key_name || s->type != SC_MAPPING_TYPE_SLIDE) continue;
                for (int j = 0; j < nkeys; j++) {
                    if (s->keys[0] == keys[j]) {
                        ERR_MSG("combo '%s' contains slide key '%s'",
                                key_name, s->key_name);
                        return false;
                    }
                }
            }
        }
    }

    struct sc_mapping_entry *e = find_by_name_impl(mm, key_name);
    if (!e) {
        int slot = find_free_slot(mm);
        if (slot < 0) {
            ERR_MSG("mapping table full (%d entries)", SC_MAPPING_MAX_ENTRIES);
            return false;
        }
        e = &mm->entries[slot];
        entry_clear(e);
        e->key_name = malloc(strlen(key_name) + 1);
        if (!e->key_name) {
            ERR_MSG("out of memory");
            return false;
        }
        strcpy(e->key_name, key_name);
        e->pointer_id = acquire_pid(mm);
        mm->count++;
    }

    memcpy(e->keys, keys, sizeof(keys));
    e->nkeys = is_mouse ? 0 : nkeys;
    e->is_mouse = is_mouse;
    e->type = is_mouse ? SC_MAPPING_TYPE_TOUCH : type;
    e->nx = nx;
    e->ny = ny;
    e->ndx = (type == SC_MAPPING_TYPE_SLIDE) ? ndx : 0.0;
    e->ndy = (type == SC_MAPPING_TYPE_SLIDE) ? ndy : 0.0;
    e->duration_ms = e->type == SC_MAPPING_TYPE_SLIDE ? duration_ms : 0;
    e->down = false;
    e->active = false;

    refresh_mouse_indices(mm);
    mm->dirty = true;
    return true;
}

bool
sc_mapping_manager_remove_entry(struct sc_mapping_manager *mm,
                                const char *key_name) {
    struct sc_mapping_entry *e = find_by_name_impl(mm, key_name);
    if (!e) return false;

    // Lift any finger still held by this entry before freeing it.
    if (mm->release_hook) {
        mm->release_hook(mm->release_opaque);
    }
    release_pid(mm, e->pointer_id);
    entry_clear(e);
    recount(mm);
    refresh_mouse_indices(mm);
    mm->dirty = true;
    return true;
}

// ============================================================
// Design mode
// ============================================================

void
sc_mapping_manager_design_reset(struct sc_mapping_manager *mm) {
    mm->design_stage = 0;
    mm->design_dx = 0.0;
    mm->design_dy = 0.0;
}

void
sc_mapping_manager_design_click(struct sc_mapping_manager *mm,
                                double nx, double ny) {
    if (mm->design_is_slide) {
        if (mm->design_stage == 0) {
            mm->design_nx = nx;
            mm->design_ny = ny;
            mm->design_stage = 1;
        } else {
            // second click: end point
            mm->design_dx = nx - mm->design_nx;
            mm->design_dy = ny - mm->design_ny;
            mm->design_stage = 2;
        }
        return;
    }
    // single-point binding
    mm->design_nx = nx;
    mm->design_ny = ny;
    mm->design_dx = 0.0;
    mm->design_dy = 0.0;
    mm->design_stage = 1;
}

bool
sc_mapping_manager_design_has_point(struct sc_mapping_manager *mm) {
    return mm->design_stage > 0;
}

bool
sc_mapping_manager_design_bind(struct sc_mapping_manager *mm,
                               const char *key_name) {
    if (mm->design_stage == 0) return false;
    if (!key_name || !*key_name) return false;

    char err[128];
    enum sc_mapping_entry_type type = SC_MAPPING_TYPE_TOUCH;
    double ndx = 0.0, ndy = 0.0;
    int duration = 0;

    if (mm->design_is_slide) {
        if (mm->design_stage < 2) return false; // need both points
        type = SC_MAPPING_TYPE_SLIDE;
        ndx = mm->design_dx;
        ndy = mm->design_dy;
        duration = mm->design_duration_ms
                 ? mm->design_duration_ms : SLIDE_DURATION_DEFAULT_MS;
    }

    bool ok = sc_mapping_manager_upsert(mm, key_name, type,
                                        mm->design_nx, mm->design_ny,
                                        ndx, ndy, duration, err, sizeof(err));
    if (ok) {
        sc_mapping_manager_design_reset(mm);
    } else {
        LOGW("Design bind rejected: %s", err);
    }
    return ok;
}

// ============================================================
// Lifecycle
// ============================================================

void
sc_mapping_manager_set_release_hook(struct sc_mapping_manager *mm,
                                    void (*cb)(void *userdata),
                                    void *userdata) {
    mm->release_hook = cb;
    mm->release_opaque = userdata;
}

bool
sc_mapping_manager_init(struct sc_mapping_manager *mm) {
    memset(mm, 0, sizeof(*mm));
    mm->mouse_right_idx = -1;
    mm->mouse_left_locked_idx = -1;
    mm->pid_cursor = 0;
    mm->design_duration_ms = SLIDE_DURATION_DEFAULT_MS;

    // Defaults: mapping disabled until a template/config says otherwise, and
    // nothing is hijacked (no W, no right click) without an explicit entry.
    mm->enabled = false;
    mm->sc_switch = SDLK_F2;
    mm->sc_design = SDLK_F3;
    mm->sc_toggle = SDLK_F4;
    mm->sc_manager = SDLK_F5;

    char *base = sc_mapping_get_base_dir();
    mm->templates_dir = join_path(base, "templates/");
    mm->config_path = join_path(base, "scrcpy_mapping_config.json");
    SDL_free(base);

    if (!mm->templates_dir || !mm->config_path) {
        LOGE("Could not resolve mapping paths");
        return false;
    }

    load_config(mm);

    if (!mm->active_name) {
        mm->active_name = strdup("default");
    }

    if (!load_template_into_mm(mm, mm->active_name)) {
        LOGW("Active template '%s' not found: mapping disabled",
             mm->active_name);
        mm->enabled = false;
    }

    return true;
}

void
sc_mapping_manager_tick(struct sc_mapping_manager *mm) {
    apply_pending_switch(mm);
}

void
sc_mapping_manager_reload_if_changed(struct sc_mapping_manager *mm) {
    struct sc_file_stamp cs = file_stamp(mm->config_path);
    if (!stamp_equal(&cs, &mm->config_stamp)) {
        LOGI("Config changed, reloading");
        char *old_active = mm->active_name ? strdup(mm->active_name) : NULL;
        load_config(mm);
        mm->config_stamp = cs;
        if (!old_active || !mm->active_name ||
                strcmp(old_active, mm->active_name) != 0) {
            char *name = mm->active_name ? strdup(mm->active_name) : NULL;
            if (name) {
                sc_mapping_manager_switch(mm, name);
                free(name);
            }
        }
        free(old_active);
    }

    if (!mm->active_name) return;
    char *full = template_path(mm, mm->active_name);
    if (!full) return;
    struct sc_file_stamp ts = file_stamp(full);
    if (!stamp_equal(&ts, &mm->active_stamp)) {
        LOGI("Template '%s' changed, hot-reloading", mm->active_name);
        load_template_into_mm(mm, mm->active_name);
    }
    free(full);
}

void
sc_mapping_manager_destroy(struct sc_mapping_manager *mm) {
    // Lift every finger before dropping the entries (P0-3 semantics).
    if (mm->release_hook) {
        mm->release_hook(mm->release_opaque);
    }
    free_all_entries(mm);
    free(mm->active_name);
    mm->active_name = NULL;
    free(mm->pending_switch);
    mm->pending_switch = NULL;
    free(mm->templates_dir);
    mm->templates_dir = NULL;
    free(mm->config_path);
    mm->config_path = NULL;
}
