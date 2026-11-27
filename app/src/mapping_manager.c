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

// Reserved mouse entry name ("key" without a keycode). Mouse *buttons* are
// ordinary synthetic keycodes now; only the left-button lock is special.
static bool
is_mouse_left_locked(const char *name);

static bool
is_mouse_name(const char *name) {
    return is_mouse_left_locked(name);
}

// Mouse motion is not a key press: it is a reserved entry name driven by the
// mouse motion events while a button is held.
static bool
is_motion_name(const char *name) {
    return SDL_strcasecmp(name, "MOUSE_MOVE") == 0;
}

// Map a mouse button name to its SDL button number (1 = left ... 5 = X2).
static bool
parse_mouse_button_name(const char *name, int *button) {
    if (SDL_strcasecmp(name, "MOUSE_LEFT") == 0
            || SDL_strcasecmp(name, "LEFT_CLICK") == 0) {
        *button = 1;
        return true;
    }
    if (SDL_strcasecmp(name, "MOUSE_MIDDLE") == 0
            || SDL_strcasecmp(name, "MIDDLE_CLICK") == 0) {
        *button = 2;
        return true;
    }
    if (SDL_strcasecmp(name, "MOUSE_RIGHT") == 0
            || SDL_strcasecmp(name, "RIGHT_CLICK") == 0) {
        *button = 3;
        return true;
    }
    if (SDL_strcasecmp(name, "MOUSE_X1") == 0
            || SDL_strcasecmp(name, "MOUSE_BUTTON4") == 0) {
        *button = 4;
        return true;
    }
    if (SDL_strcasecmp(name, "MOUSE_X2") == 0
            || SDL_strcasecmp(name, "MOUSE_BUTTON5") == 0) {
        *button = 5;
        return true;
    }
    return false;
}

// ============================================================
// Path / file helpers
// ============================================================

char *
sc_mapping_get_base_dir(void) {
    // SDL_GetBasePath() returns a pointer owned by SDL (it caches it), so it
    // must not be freed here: only the copy handed to the caller is.
    const char *base = SDL_GetBasePath();
#ifdef _WIN32
    // The Windows build keeps the real executable (and every DLL) in the
    // hard-coded "dll" subdirectory. User-editable templates and config live
    // one level above it, in the installation root. Derive that parent path
    // from the executable location only: no environment variables, no registry.
    if (base) {
        char *parent = SDL_strdup(base);
        size_t len;
        char *sep;

        if (!parent) {
            return NULL;
        }
        len = strlen(parent);
        while (len > 0 && (parent[len - 1] == '\\' || parent[len - 1] == '/')) {
            parent[--len] = '\0';
        }
        sep = strrchr(parent, '\\');
        if (!sep) {
            sep = strrchr(parent, '/');
        }
        if (sep && sep != parent) {
            sep[1] = '\0';
            return parent;
        }
        SDL_free(parent);
    }
#endif
    if (base) {
        return SDL_strdup(base);
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

    // Mouse buttons as mappable "keys" (synthetic keycodes, see the header).
    // MOUSE_LEFT_LOCKED is a special entry, not a key, and is deliberately not
    // handled here.
    int button = 0;
    if (parse_mouse_button_name(start, &button)) {
        return SC_MAPPING_MOUSE_KEY(button);
    }

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
sc_mapping_mouse_key_name(SDL_Keycode kc) {
    if (!SC_MAPPING_KEYCODE_IS_MOUSE(kc)) return NULL;
    switch (SC_MAPPING_KEYCODE_MOUSE_BUTTON(kc)) {
        case 1: return "MOUSE_LEFT";
        case 2: return "MOUSE_MIDDLE";
        case 3: return "MOUSE_RIGHT";
        case 4: return "MOUSE_X1";
        case 5: return "MOUSE_X2";
        default: return NULL;
    }
}

const char *
sc_mapping_key_name(SDL_Keycode kc) {
    const char *mouse = sc_mapping_mouse_key_name(kc);
    if (mouse) return mouse;
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

// The key expression grammar itself lives in mapping_cond.c, next to the
// dependency reference grammar it shares ("key:" prefix).

bool
sc_mapping_validate_key_expr(const char *name, char *err, size_t errlen) {
    // Reserved names: the left-button lock entry and the motion entry carry no
    // keycode (type consistency is checked by the callers).
    if (is_mouse_name(name) || is_motion_name(name)) {
        return true;
    }
    SDL_Keycode keys[SC_MAPPING_MAX_KEYS];
    int nkeys = 0;
    return sc_cond_parse_key_expr(name, keys, &nkeys, err, errlen);
}

bool
sc_mapping_validate_cond_ref(const char *expr, char *err, size_t errlen) {
    struct sc_mapping_cond c;
    return sc_cond_parse_ref(expr, &c, err, errlen);
}

// ============================================================
// Special mouse names
// ============================================================

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
        if (is_mouse_left_locked(mm->entries[i].key_name)) {
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

// Copy the parsed dependency conditions into an entry.
static void
entry_set_deps(struct sc_mapping_entry *e, const struct sc_mapping_cond *deps,
               int ndeps) {
    if (ndeps < 0) {
        ndeps = 0;
    }
    if (ndeps > SC_COND_MAX_DEPS) {
        ndeps = SC_COND_MAX_DEPS;
    }
    if (ndeps > 0 && deps) {
        memcpy(e->deps, deps, (size_t) ndeps * sizeof(deps[0]));
    }
    e->ndeps = ndeps;
}

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

static void
cross_validate_deps(struct sc_parse_out *out, const char *path);

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

        // Never write past the fixed entry table: every further entry is
        // rejected and counted (and shown in the load statistics).
        if (out->count >= SC_MAPPING_MAX_ENTRIES) {
            stat_fail(&out->stats, path, i, "mapping table full");
            continue;
        }

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
            } else if (SDL_strcasecmp(jt->valuestring, "motion") == 0) {
                type = SC_MAPPING_TYPE_MOTION;
            } else {
                snprintf(err, sizeof(err), "unknown type '%s'",
                         jt->valuestring);
                stat_fail(&out->stats, path, i, err);
                continue;
            }
        }

        // "MOUSE_MOVE" is the only key that can carry the motion type, and the
        // motion type is the only one that accepts it.
        bool motion_name = is_motion_name(key_name);
        if (type == SC_MAPPING_TYPE_MOTION && !motion_name) {
            stat_fail(&out->stats, path, i,
                      "type 'motion' requires key 'MOUSE_MOVE'");
            continue;
        }
        if (motion_name && type != SC_MAPPING_TYPE_MOTION) {
            stat_fail(&out->stats, path, i,
                      "'MOUSE_MOVE' requires type 'motion'");
            continue;
        }

        // "virtual": a logical key with no touch injection. It only tracks
        // press/release (or toggle) state so other entries can depend on it or
        // include it in a combo. x/y are ignored and not required.
        bool is_virtual = false;
        cJSON *jvirt = cJSON_GetObjectItemCaseSensitive(item, "virtual");
        if (jvirt) {
            if (!cJSON_IsBool(jvirt)) {
                stat_fail(&out->stats, path, i, "'virtual' must be a boolean");
                continue;
            }
            is_virtual = cJSON_IsTrue(jvirt);
            if (is_virtual && (motion_name || is_mouse_name(key_name))) {
                stat_fail(&out->stats, path, i,
                          "reserved names cannot be virtual");
                continue;
            }
            if (is_virtual && type != SC_MAPPING_TYPE_TOUCH) {
                stat_fail(&out->stats, path, i,
                          "virtual keys only support type 'touch'");
                continue;
            }
        }

        // x/y: required for touch and slide (unless virtual). For motion they
        // are optional and only define the re-anchor point (default: screen
        // center). For virtual keys x/y are accepted but never used.
        cJSON *jx = cJSON_GetObjectItemCaseSensitive(item, "x");
        cJSON *jy = cJSON_GetObjectItemCaseSensitive(item, "y");
        double nx = 0.5;
        double ny = 0.5;
        if (cJSON_IsNumber(jx) && cJSON_IsNumber(jy)) {
            nx = jx->valuedouble;
            ny = jy->valuedouble;
            if (nx < 0.0 || nx > 1.0 || ny < 0.0 || ny > 1.0) {
                snprintf(err, sizeof(err),
                         "'x'/'y' out of range [0,1] (%f, %f)", nx, ny);
                stat_fail(&out->stats, path, i, err);
                continue;
            }
        } else if (jx || jy) {
            // One of them present but not both numeric: never guess.
            stat_fail(&out->stats, path, i,
                      "'x'/'y' must both be numbers");
            continue;
        } else if (type != SC_MAPPING_TYPE_MOTION && !is_virtual) {
            stat_fail(&out->stats, path, i, "missing or invalid 'x'/'y'");
            continue;
        }

        // "positions": optional array of [x, y] pairs for multi-touch. The
        // primary position (x/y above) is index 0; these are the extras.
        // Virtual keys ignore positions (they have no screen presence).
        int npositions = 1;
        double extra_nx[SC_MAPPING_MAX_POSITIONS - 1] = { 0 };
        double extra_ny[SC_MAPPING_MAX_POSITIONS - 1] = { 0 };
        cJSON *jpos = cJSON_GetObjectItemCaseSensitive(item, "positions");
        if (jpos) {
            if (is_virtual) {
                stat_fail(&out->stats, path, i,
                          "virtual keys cannot have 'positions'");
                continue;
            }
            if (!cJSON_IsArray(jpos)) {
                stat_fail(&out->stats, path, i, "'positions' must be an array");
                continue;
            }
            int np = cJSON_GetArraySize(jpos);
            if (np > SC_MAPPING_MAX_POSITIONS - 1) {
                snprintf(err, sizeof(err),
                         "too many positions (%d > %d extras)",
                         np, SC_MAPPING_MAX_POSITIONS - 1);
                stat_fail(&out->stats, path, i, err);
                continue;
            }
            bool ok = true;
            for (int p = 0; p < np; p++) {
                cJSON *pair = cJSON_GetArrayItem(jpos, p);
                if (!cJSON_IsArray(pair) || cJSON_GetArraySize(pair) != 2) {
                    stat_fail(&out->stats, path, i,
                              "each position must be a [x, y] pair");
                    ok = false;
                    break;
                }
                cJSON *px = cJSON_GetArrayItem(pair, 0);
                cJSON *py = cJSON_GetArrayItem(pair, 1);
                if (!cJSON_IsNumber(px) || !cJSON_IsNumber(py)) {
                    stat_fail(&out->stats, path, i,
                              "position coordinates must be numbers");
                    ok = false;
                    break;
                }
                double vx = px->valuedouble;
                double vy = py->valuedouble;
                if (vx < 0.0 || vx > 1.0 || vy < 0.0 || vy > 1.0) {
                    snprintf(err, sizeof(err),
                             "position out of range [0,1] (%f, %f)", vx, vy);
                    stat_fail(&out->stats, path, i, err);
                    ok = false;
                    break;
                }
                extra_nx[p] = vx;
                extra_ny[p] = vy;
            }
            if (!ok) continue;
            npositions = np + 1;
        }

        // mode (optional, defaults to "momentary")
        enum sc_mapping_mode mode = SC_MAPPING_MODE_MOMENTARY;
        cJSON *jmode = cJSON_GetObjectItemCaseSensitive(item, "mode");
        if (jmode && !cJSON_IsString(jmode)) {
            stat_fail(&out->stats, path, i, "'mode' must be a string");
            continue;
        }
        if (cJSON_IsString(jmode)) {
            if (SDL_strcasecmp(jmode->valuestring, "momentary") == 0) {
                mode = SC_MAPPING_MODE_MOMENTARY;
            } else if (SDL_strcasecmp(jmode->valuestring, "toggle") == 0) {
                mode = SC_MAPPING_MODE_TOGGLE;
            } else {
                snprintf(err, sizeof(err), "unknown mode '%s'",
                         jmode->valuestring);
                stat_fail(&out->stats, path, i, err);
                continue;
            }
        }

        // Dependency (optional). "depends_on" accepts:
        //   - a string: a key / mouse button / key combo ("lshift", "A+B"),
        //     an entry reference ("entry:VIRT") or a feature flag
        //     ("flag:look_mode");
        //   - an array of strings: several conditions, all of which must hold
        //     (logical AND).
        // "depends_state" is a string (applied to every condition) or an array
        // of strings (one per condition). Default: "pressed" (i.e. the source
        // must be held / active / on).
        struct sc_mapping_cond deps[SC_COND_MAX_DEPS];
        int ndeps = 0;
        cJSON *jdep = cJSON_GetObjectItemCaseSensitive(item, "depends_on");
        cJSON *jdepstate = cJSON_GetObjectItemCaseSensitive(item,
                                                           "depends_state");
        if (jdep) {
            if (cJSON_IsString(jdep)) {
                if (!jdep->valuestring || !*jdep->valuestring) {
                    stat_fail(&out->stats, path, i,
                              "missing or invalid 'depends_on'");
                    continue;
                }
                if (!sc_cond_parse_ref(jdep->valuestring, &deps[0], err,
                                       sizeof(err))) {
                    stat_fail(&out->stats, path, i, err);
                    continue;
                }
                ndeps = 1;
            } else if (cJSON_IsArray(jdep)) {
                int n = cJSON_GetArraySize(jdep);
                if (n < 1) {
                    stat_fail(&out->stats, path, i,
                              "'depends_on' array must not be empty");
                    continue;
                }
                if (n > SC_COND_MAX_DEPS) {
                    snprintf(err, sizeof(err),
                             "too many dependencies (%d > %d)", n,
                             SC_COND_MAX_DEPS);
                    stat_fail(&out->stats, path, i, err);
                    continue;
                }
                bool dep_ok_parse = true;
                for (int d = 0; d < n; d++) {
                    cJSON *jd = cJSON_GetArrayItem(jdep, d);
                    if (!cJSON_IsString(jd) || !jd->valuestring
                            || !*jd->valuestring) {
                        stat_fail(&out->stats, path, i,
                                  "each dependency must be a non-empty string");
                        dep_ok_parse = false;
                        break;
                    }
                    if (!sc_cond_parse_ref(jd->valuestring, &deps[d], err,
                                           sizeof(err))) {
                        stat_fail(&out->stats, path, i, err);
                        dep_ok_parse = false;
                        break;
                    }
                }
                if (!dep_ok_parse) {
                    continue;
                }
                ndeps = n;
            } else {
                stat_fail(&out->stats, path, i,
                          "'depends_on' must be a string or an array of strings");
                continue;
            }

            // "depends_state": a single state for every condition, or one
            // state per condition.
            if (jdepstate) {
                if (cJSON_IsString(jdepstate)) {
                    enum sc_mapping_cond_state st;
                    if (!sc_cond_parse_state(jdepstate->valuestring, &st, err,
                                             sizeof(err))) {
                        stat_fail(&out->stats, path, i, err);
                        continue;
                    }
                    for (int d = 0; d < ndeps; d++) {
                        deps[d].state = st;
                    }
                } else if (cJSON_IsArray(jdepstate)) {
                    int n = cJSON_GetArraySize(jdepstate);
                    if (n != ndeps) {
                        snprintf(err, sizeof(err),
                                 "'depends_state' has %d entries but "
                                 "'depends_on' has %d", n, ndeps);
                        stat_fail(&out->stats, path, i, err);
                        continue;
                    }
                    bool state_ok = true;
                    for (int d = 0; d < n; d++) {
                        cJSON *js = cJSON_GetArrayItem(jdepstate, d);
                        if (!cJSON_IsString(js)) {
                            stat_fail(&out->stats, path, i,
                                      "'depends_state' entries must be strings");
                            state_ok = false;
                            break;
                        }
                        enum sc_mapping_cond_state st;
                        if (!sc_cond_parse_state(js->valuestring, &st, err,
                                                 sizeof(err))) {
                            stat_fail(&out->stats, path, i, err);
                            state_ok = false;
                            break;
                        }
                        deps[d].state = st;
                    }
                    if (!state_ok) {
                        continue;
                    }
                } else {
                    stat_fail(&out->stats, path, i,
                              "'depends_state' must be a string or an array");
                    continue;
                }
            }
        } else if (jdepstate) {
            stat_fail(&out->stats, path, i,
                      "'depends_state' requires 'depends_on'");
            continue;
        }

        // sensitivity (optional, motion only)
        double sensitivity = SC_MAPPING_SENSITIVITY_DEFAULT;
        cJSON *jsens = cJSON_GetObjectItemCaseSensitive(item, "sensitivity");
        if (jsens) {
            if (!cJSON_IsNumber(jsens)) {
                stat_fail(&out->stats, path, i,
                          "'sensitivity' must be a number");
                continue;
            }
            if (type != SC_MAPPING_TYPE_MOTION) {
                stat_fail(&out->stats, path, i,
                          "'sensitivity' is only valid for type 'motion'");
                continue;
            }
            sensitivity = jsens->valuedouble;
            if (sensitivity < SC_MAPPING_SENSITIVITY_MIN) {
                LOGW("Template '%s' entry %d: sensitivity %f < %f, clamped",
                     path, i, sensitivity, SC_MAPPING_SENSITIVITY_MIN);
                sensitivity = SC_MAPPING_SENSITIVITY_MIN;
            } else if (sensitivity > SC_MAPPING_SENSITIVITY_MAX) {
                LOGW("Template '%s' entry %d: sensitivity %f > %f, clamped",
                     path, i, sensitivity, SC_MAPPING_SENSITIVITY_MAX);
                sensitivity = SC_MAPPING_SENSITIVITY_MAX;
            }
        }

        // hold (optional, motion only): "button" (default) keeps the finger
        // down only while a mouse button is held; "always" is the look mode
        // where the finger stays down from the first movement until the
        // mapping is stopped.
        enum sc_motion_hold hold = SC_MOTION_HOLD_BUTTON;
        cJSON *jhold = cJSON_GetObjectItemCaseSensitive(item, "hold");
        if (jhold) {
            if (!cJSON_IsString(jhold)) {
                stat_fail(&out->stats, path, i,
                          "'hold' must be a string");
                continue;
            }
            if (type != SC_MAPPING_TYPE_MOTION) {
                stat_fail(&out->stats, path, i,
                          "'hold' is only valid for type 'motion'");
                continue;
            }
            if (SDL_strcasecmp(jhold->valuestring, "button") == 0) {
                hold = SC_MOTION_HOLD_BUTTON;
            } else if (SDL_strcasecmp(jhold->valuestring, "always") == 0) {
                hold = SC_MOTION_HOLD_ALWAYS;
            } else {
                snprintf(err, sizeof(err), "unknown hold '%s'",
                         jhold->valuestring);
                stat_fail(&out->stats, path, i, err);
                continue;
            }
        }

        // Mouse reserved names (the left-button lock is a special entry)
        if (is_mouse_name(key_name)) {
            if (type != SC_MAPPING_TYPE_TOUCH) {
                snprintf(err, sizeof(err), "'%s' cannot be a %s", key_name,
                         type == SC_MAPPING_TYPE_SLIDE ? "slide" : "motion");
                stat_fail(&out->stats, path, i, err);
                continue;
            }
            if (mode != SC_MAPPING_MODE_MOMENTARY) {
                snprintf(err, sizeof(err),
                         "'%s' cannot use 'mode: toggle'", key_name);
                stat_fail(&out->stats, path, i, err);
                continue;
            }
            if (ndeps > 0) {
                snprintf(err, sizeof(err),
                         "'%s' cannot use 'depends_on'", key_name);
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
            e->is_virtual = false;
            e->type = SC_MAPPING_TYPE_TOUCH;
            e->nx = nx;
            e->ny = ny;
            e->npositions = 1;
            e->duration_ms = 0;
            e->sensitivity = SC_MAPPING_SENSITIVITY_DEFAULT;
            out->count++;
            out->stats.ok++;
            continue;
        }

        // Mouse motion entry: no keycode, driven by mouse motion events.
        if (type == SC_MAPPING_TYPE_MOTION) {
            if (mode != SC_MAPPING_MODE_MOMENTARY) {
                stat_fail(&out->stats, path, i,
                          "'mode: toggle' is not supported for motion");
                continue;
            }
            struct sc_mapping_entry *e = &out->entries[out->count];
            e->key_name = strdup(key_name);
            if (!e->key_name) {
                stat_fail(&out->stats, path, i, "out of memory");
                continue;
            }
            e->nkeys = 0;
            e->is_virtual = false;
            e->type = SC_MAPPING_TYPE_MOTION;
            e->nx = nx;
            e->ny = ny;
            e->npositions = 1;
            e->sensitivity = sensitivity;
            e->hold = hold;
            entry_set_deps(e, deps, ndeps);
            out->count++;
            out->stats.ok++;
            continue;
        }

        SDL_Keycode keys[SC_MAPPING_MAX_KEYS];
        int nkeys = 0;
        if (!sc_cond_parse_key_expr(key_name, keys, &nkeys, err, sizeof(err))) {
            stat_fail(&out->stats, path, i, err);
            continue;
        }

        // A toggle flips on every press edge, which is only well defined for a
        // single trigger: reject it on combos instead of guessing.
        if (mode == SC_MAPPING_MODE_TOGGLE && nkeys != 1) {
            snprintf(err, sizeof(err),
                     "'mode: toggle' requires a single key, not a combo ('%s')",
                     key_name);
            stat_fail(&out->stats, path, i, err);
            continue;
        }
        // Toggle is only meaningful for momentary touch presses; a slide has
        // its own press/release cycle. Reject instead of silently dropping the
        // field on the next save.
        if (mode == SC_MAPPING_MODE_TOGGLE && type == SC_MAPPING_TYPE_SLIDE) {
            snprintf(err, sizeof(err),
                     "'mode: toggle' is not supported for slides ('%s')",
                     key_name);
            stat_fail(&out->stats, path, i, err);
            continue;
        }
        // A single-key entry may not depend on its own trigger key.
        if (ndeps > 0 && nkeys == 1) {
            bool self_dep = false;
            for (int di = 0; di < ndeps && !self_dep; di++) {
                if (deps[di].kind != SC_COND_KIND_KEY) {
                    continue;
                }
                for (int dj = 0; dj < deps[di].nkeys; dj++) {
                    if (deps[di].keys[dj] == keys[0]) {
                        self_dep = true;
                        break;
                    }
                }
            }
            if (self_dep) {
                snprintf(err, sizeof(err), "'%s' depends on itself", key_name);
                stat_fail(&out->stats, path, i, err);
                continue;
            }
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
            e->is_virtual = false;
            e->type = SC_MAPPING_TYPE_SLIDE;
            e->nx = nx;
            e->ny = ny;
            e->npositions = 1;
            e->ndx = ndx;
            e->ndy = ndy;
            e->duration_ms = duration_ms;
            e->sensitivity = SC_MAPPING_SENSITIVITY_DEFAULT;
            entry_set_deps(e, deps, ndeps);
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
        e->is_virtual = is_virtual;
        e->type = SC_MAPPING_TYPE_TOUCH;
        e->mode = mode;
        e->nx = is_virtual ? 0.0 : nx;
        e->ny = is_virtual ? 0.0 : ny;
        e->npositions = is_virtual ? 1 : npositions;
        if (!is_virtual) {
            memcpy(e->extra_nx, extra_nx, sizeof(extra_nx));
            memcpy(e->extra_ny, extra_ny, sizeof(extra_ny));
        }
        e->duration_ms = 0;
        e->sensitivity = SC_MAPPING_SENSITIVITY_DEFAULT;
        entry_set_deps(e, deps, ndeps);
        out->count++;
        out->stats.ok++;
    }

    if (out->count >= SC_MAPPING_MAX_ENTRIES) {
        LOGW("Template '%s': entry table full, extra entries ignored", path);
    }

    cross_validate_slides(out, path);
    cross_validate_deps(out, path);

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

// Compact the entry table after a rejection (drop the holes).
static void
compact_entries(struct sc_parse_out *out) {
    int n = 0;
    for (int i = 0; i < out->count; i++) {
        if (!out->entries[i].key_name) continue;
        if (n != i) out->entries[n] = out->entries[i];
        n++;
    }
    out->count = n;
}

// Find the entry named "name" among the parsed entries (NULL when unknown).
static struct sc_mapping_entry *
find_parsed_by_name(struct sc_parse_out *out, const char *name) {
    if (!name || !*name) {
        return NULL;
    }
    for (int i = 0; i < out->count; i++) {
        struct sc_mapping_entry *e = &out->entries[i];
        if (e->key_name && SDL_strcasecmp(e->key_name, name) == 0) {
            return e;
        }
    }
    return NULL;
}

// True when the dependency chain that starts at "name" reaches "target"
// (following every entry condition of every visited entry).
static bool
dep_chain_reaches(struct sc_parse_out *out, const char *name,
                  const char *target, int depth) {
    if (depth > SC_MAPPING_MAX_ENTRIES) {
        return false; // defensive: the table is bounded
    }
    struct sc_mapping_entry *e = find_parsed_by_name(out, name);
    if (!e) {
        return false;
    }
    for (int d = 0; d < e->ndeps; d++) {
        if (e->deps[d].kind != SC_COND_KIND_ENTRY) {
            continue;
        }
        if (SDL_strcasecmp(e->deps[d].name, target) == 0) {
            return true;
        }
        if (dep_chain_reaches(out, e->deps[d].name, target, depth + 1)) {
            return true;
        }
    }
    return false;
}

// Cross-validate the dependency conditions that reference another entry:
//   - the referenced entry must exist in the same template;
//   - an entry may not depend on itself;
//   - dependency chains must not form a cycle (A depends on B, B on A).
// A rejected entry is dropped, like an entry rejected by the field checks.
static void
cross_validate_deps(struct sc_parse_out *out, const char *path) {
    for (int i = 0; i < out->count; i++) {
        struct sc_mapping_entry *e = &out->entries[i];
        if (!e->key_name || e->ndeps <= 0) {
            continue;
        }
        bool reject = false;
        char reason[128] = { 0 };
        for (int d = 0; d < e->ndeps && !reject; d++) {
            if (e->deps[d].kind != SC_COND_KIND_ENTRY) {
                continue;
            }
            const char *target = e->deps[d].name;
            if (SDL_strcasecmp(target, e->key_name) == 0) {
                snprintf(reason, sizeof(reason),
                         "'%s' depends on itself", e->key_name);
                reject = true;
                break;
            }
            if (!find_parsed_by_name(out, target)) {
                snprintf(reason, sizeof(reason),
                         "unknown dependency entry '%s'", target);
                reject = true;
                break;
            }
            if (dep_chain_reaches(out, target, e->key_name, 0)) {
                snprintf(reason, sizeof(reason),
                         "dependency cycle through '%s'", target);
                reject = true;
                break;
            }
        }
        if (reject) {
            stat_fail(&out->stats, path, i, reason);
            free(e->key_name);
            e->key_name = NULL;
        }
    }
    // compact
    compact_entries(out);
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
    compact_entries(out);
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
        if (!mm->entries[i].is_virtual) {
            mm->entries[i].pointer_id = acquire_pid(mm);
            for (int p = 0; p < mm->entries[i].npositions - 1; p++) {
                mm->entries[i].extra_pid[p] = acquire_pid(mm);
            }
        }
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
        // Virtual keys have no screen position: omit x/y entirely.
        if (!e->is_virtual) {
            cJSON_AddNumberToObject(item, "x", e->nx);
            cJSON_AddNumberToObject(item, "y", e->ny);
            // Extra multi-touch positions
            if (e->npositions > 1) {
                cJSON *pos = cJSON_CreateArray();
                for (int p = 0; p < e->npositions - 1; p++) {
                    cJSON *pair = cJSON_CreateArray();
                    cJSON_AddItemToArray(pair,
                                         cJSON_CreateNumber(e->extra_nx[p]));
                    cJSON_AddItemToArray(pair,
                                         cJSON_CreateNumber(e->extra_ny[p]));
                    cJSON_AddItemToArray(pos, pair);
                }
                cJSON_AddItemToObject(item, "positions", pos);
            }
        } else {
            cJSON_AddBoolToObject(item, "virtual", true);
        }
        if (e->type == SC_MAPPING_TYPE_SLIDE) {
            cJSON_AddStringToObject(item, "type", "slide");
            cJSON_AddNumberToObject(item, "dx", e->ndx);
            cJSON_AddNumberToObject(item, "dy", e->ndy);
            cJSON_AddNumberToObject(item, "duration_ms", e->duration_ms);
        } else if (e->type == SC_MAPPING_TYPE_MOTION) {
            cJSON_AddStringToObject(item, "type", "motion");
            cJSON_AddNumberToObject(item, "sensitivity", e->sensitivity);
            if (e->hold == SC_MOTION_HOLD_ALWAYS) {
                cJSON_AddStringToObject(item, "hold", "always");
            }
        }
        // Optional state fields are written only when they differ from the
        // defaults, so an untouched template keeps its original shape
        // (load -> save -> load stays idempotent either way).
        if (e->mode == SC_MAPPING_MODE_TOGGLE) {
            cJSON_AddStringToObject(item, "mode", "toggle");
        }
        // Dependency: one condition -> a plain string, several -> an array.
        // The state is written with the word matching the source kind
        // ("pressed" for a key, "active" for an entry, "on" for a flag).
        if (e->ndeps > 0) {
            char depname[128];
            if (e->ndeps == 1) {
                sc_cond_to_name(&e->deps[0], depname, sizeof(depname));
                cJSON_AddStringToObject(item, "depends_on", depname);
                cJSON_AddStringToObject(item, "depends_state",
                                        sc_cond_state_name(&e->deps[0]));
            } else {
                cJSON *deps = cJSON_CreateArray();
                cJSON *states = cJSON_CreateArray();
                for (int d = 0; d < e->ndeps; d++) {
                    sc_cond_to_name(&e->deps[d], depname, sizeof(depname));
                    cJSON_AddItemToArray(deps, cJSON_CreateString(depname));
                    cJSON_AddItemToArray(states,
                                         cJSON_CreateString(
                                             sc_cond_state_name(&e->deps[d])));
                }
                cJSON_AddItemToObject(item, "depends_on", deps);
                cJSON_AddItemToObject(item, "depends_state", states);
            }
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
    return sc_mapping_manager_upsert_ex(mm, key_name, type, nx, ny, ndx, ndy,
                                        duration_ms, SC_MAPPING_MODE_MOMENTARY,
                                        NULL, 0,
                                        SC_MAPPING_SENSITIVITY_DEFAULT,
                                        SC_MOTION_HOLD_BUTTON, false,
                                        err, errlen);
}

bool
sc_mapping_manager_upsert_ex(struct sc_mapping_manager *mm, const char *key_name,
                             enum sc_mapping_entry_type type, double nx,
                             double ny, double ndx, double ndy, int duration_ms,
                             enum sc_mapping_mode mode,
                             const struct sc_mapping_cond *deps, int ndeps,
                             double sensitivity, enum sc_motion_hold hold,
                             bool is_virtual,
                             char *err, size_t errlen) {
    if (!key_name || !*key_name) {
        ERR_MSG("empty key name");
        return false;
    }

    bool is_mouse = is_mouse_name(key_name);
    bool is_motion = is_motion_name(key_name);

    // A virtual key performs no injection: it only tracks logical state so it
    // can be depended on or used as a combo member. Position, type (beyond
    // touch) and pointer id are meaningless for it.
    if (is_virtual) {
        if (is_mouse || is_motion) {
            ERR_MSG("'%s' cannot be a virtual key", key_name);
            return false;
        }
        if (type != SC_MAPPING_TYPE_TOUCH) {
            ERR_MSG("virtual keys only support type 'touch'");
            return false;
        }
        type = SC_MAPPING_TYPE_TOUCH;
    }

    // Type / key consistency (same rules as the JSON loader).
    if (is_motion && type != SC_MAPPING_TYPE_MOTION) {
        ERR_MSG("'MOUSE_MOVE' requires type 'motion'");
        return false;
    }
    if (type == SC_MAPPING_TYPE_MOTION && !is_motion) {
        ERR_MSG("type 'motion' requires key 'MOUSE_MOVE'");
        return false;
    }
    if (is_mouse && type != SC_MAPPING_TYPE_TOUCH) {
        ERR_MSG("mouse lock entries cannot be slides or motion");
        return false;
    }
    if (mode == SC_MAPPING_MODE_TOGGLE && type != SC_MAPPING_TYPE_TOUCH) {
        ERR_MSG("'mode: toggle' is only supported for touch entries");
        return false;
    }
    if (mode == SC_MAPPING_MODE_TOGGLE && is_mouse) {
        ERR_MSG("'mode: toggle' is not supported for this entry");
        return false;
    }
    if (!is_virtual && (nx < 0.0 || nx > 1.0 || ny < 0.0 || ny > 1.0)) {
        ERR_MSG("'x'/'y' out of range [0,1]");
        return false;
    }
    if (is_mouse && ndeps > 0) {
        ERR_MSG("mouse lock entries cannot use a dependency");
        return false;
    }
    if (ndeps < 0 || ndeps > SC_COND_MAX_DEPS) {
        ERR_MSG("invalid dependency count (%d)", ndeps);
        return false;
    }
    if (type != SC_MAPPING_TYPE_MOTION) {
        hold = SC_MOTION_HOLD_BUTTON; // not meaningful for touch / slide
    }

    SDL_Keycode keys[SC_MAPPING_MAX_KEYS];
    int nkeys = 0;
    if (!is_mouse && !is_motion
            && !sc_cond_parse_key_expr(key_name, keys, &nkeys, err, errlen)) {
        return false;
    }
    if (mode == SC_MAPPING_MODE_TOGGLE && nkeys != 1) {
        ERR_MSG("'mode: toggle' requires a single key");
        return false;
    }

    // Dependency conditions: a key, a combo of keys, another mapping entry or
    // a built-in feature flag. A single-key entry may never depend on its own
    // trigger key.
    if (ndeps > 0 && nkeys == 1) {
        for (int i = 0; i < ndeps; i++) {
            if (deps[i].kind != SC_COND_KIND_KEY) {
                continue;
            }
            for (int j = 0; j < deps[i].nkeys; j++) {
                if (deps[i].keys[j] == keys[0]) {
                    ERR_MSG("'%s' depends on itself", key_name);
                    return false;
                }
            }
        }
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
    if (sensitivity < SC_MAPPING_SENSITIVITY_MIN) {
        sensitivity = SC_MAPPING_SENSITIVITY_MIN;
    } else if (sensitivity > SC_MAPPING_SENSITIVITY_MAX) {
        sensitivity = SC_MAPPING_SENSITIVITY_MAX;
    }

    // A slide key must not be used in a combo (and vice versa): the slide owns
    // a pointer id, a combo press would tap with the same id.
    if (!is_mouse && !is_motion) {
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
        if (!is_virtual) {
            e->pointer_id = acquire_pid(mm);
        }
        mm->count++;
    } else if (e->active || e->down) {
        // Editing a live entry must not leave a finger pressed with the old
        // coordinates: lift everything first (the hook also stops slides).
        if (mm->release_hook) {
            mm->release_hook(mm->release_opaque);
        }
    }

    memcpy(e->keys, keys, sizeof(keys));
    e->nkeys = (is_mouse || is_motion) ? 0 : nkeys;
    e->is_mouse = is_mouse;
    e->is_virtual = is_virtual;
    e->type = type;
    e->mode = (is_mouse || is_motion) ? SC_MAPPING_MODE_MOMENTARY : mode;
    e->nx = is_virtual ? 0.0 : nx;
    e->ny = is_virtual ? 0.0 : ny;
    e->npositions = 1; // upsert_ex only creates single-position entries
    e->ndx = (type == SC_MAPPING_TYPE_SLIDE) ? ndx : 0.0;
    e->ndy = (type == SC_MAPPING_TYPE_SLIDE) ? ndy : 0.0;
    e->duration_ms = e->type == SC_MAPPING_TYPE_SLIDE ? duration_ms : 0;
    e->sensitivity = e->type == SC_MAPPING_TYPE_MOTION ? sensitivity
                                                       : SC_MAPPING_SENSITIVITY_DEFAULT;
    e->hold = e->type == SC_MAPPING_TYPE_MOTION ? hold
                                                : SC_MOTION_HOLD_BUTTON;
    entry_set_deps(e, deps, is_mouse ? 0 : ndeps);
    e->down = false;
    e->active = false;

    refresh_mouse_indices(mm);
    mm->dirty = true;
    return true;
}

bool
sc_mapping_manager_key_is_dependency(struct sc_mapping_manager *mm,
                                     SDL_Keycode kc) {
    // Only key conditions need the physical key state to be tracked. Entry
    // and flag conditions read a state that is maintained on its own.
    for (int i = 0; i < SC_MAPPING_MAX_ENTRIES; i++) {
        struct sc_mapping_entry *e = &mm->entries[i];
        if (!e->key_name) {
            continue;
        }
        for (int d = 0; d < e->ndeps; d++) {
            if (e->deps[d].kind != SC_COND_KIND_KEY) {
                continue;
            }
            for (int j = 0; j < e->deps[d].nkeys; j++) {
                if (e->deps[d].keys[j] == kc) return true;
            }
        }
    }
    return false;
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
    for (int i = 0; i < e->npositions - 1; i++) {
        release_pid(mm, e->extra_pid[i]);
    }
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

    // Re-binding an existing key must not silently reset its optional fields
    // (mode / dependency / sensitivity): keep them, only the geometry changes.
    struct sc_mapping_entry *old = find_by_name_impl(mm, key_name);
    enum sc_mapping_mode mode = old ? old->mode : SC_MAPPING_MODE_MOMENTARY;
    struct sc_mapping_cond deps[SC_COND_MAX_DEPS];
    int ndeps = 0;
    if (old) {
        ndeps = old->ndeps;
        memcpy(deps, old->deps, sizeof(deps));
    }
    double sensitivity = old ? old->sensitivity : SC_MAPPING_SENSITIVITY_DEFAULT;
    enum sc_motion_hold hold = old ? old->hold : SC_MOTION_HOLD_BUTTON;
    bool is_virtual = old ? old->is_virtual : false;
    if (type == SC_MAPPING_TYPE_SLIDE) {
        mode = SC_MAPPING_MODE_MOMENTARY; // toggle is not valid for slides
    }
    if (type != SC_MAPPING_TYPE_MOTION) {
        hold = SC_MOTION_HOLD_BUTTON;
    }

    bool ok = sc_mapping_manager_upsert_ex(mm, key_name, type,
                                           mm->design_nx, mm->design_ny,
                                           ndx, ndy, duration, mode,
                                           deps, ndeps, sensitivity, hold,
                                           is_virtual,
                                           err, sizeof(err));
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
