#ifndef SC_MAPPING_MANAGER_H
#define SC_MAPPING_MANAGER_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL_keycode.h>

#include "util/tick.h"

#define SC_MAPPING_MAX_KEYS 4
// Fixed-capacity entry table: realloc is forbidden, because other modules keep
// indices (and index-based lookups) into this array across calls.
#define SC_MAPPING_MAX_ENTRIES 256
// Reserved pointer ids used by scrcpy for the physical mouse / virtual finger.
#define SC_MAPPING_PID_MIN 4  // first free id is -(4)
#define SC_MAPPING_PID_MAX (SC_MAPPING_MAX_ENTRIES + 3)

#define SC_MAPPING_MAX_FAILS 16

enum sc_mapping_entry_type {
    SC_MAPPING_TYPE_TOUCH = 0,   // tap: press at (nx, ny)
    SC_MAPPING_TYPE_SLIDE = 1,   // hold-to-slide: from (nx, ny) by (ndx, ndy)
};

// Result of the latest template load, exposed for logging and for the overlay.
struct sc_mapping_load_stats {
    int ok;
    int fail;
    int nmsgs;
    char msgs[SC_MAPPING_MAX_FAILS][192];
};

// A single mapping entry: one key (or a key combo) mapped to a normalized
// position (nx, ny in [0.0, 1.0]), optionally a continuous slide.
struct sc_mapping_entry {
    char *key_name;                        // NULL means the slot is free
    SDL_Keycode keys[SC_MAPPING_MAX_KEYS]; // parsed keycodes (empty for mouse)
    int nkeys;                             // 0 = special mouse entry
    enum sc_mapping_entry_type type;
    double nx;
    double ny;
    double ndx;        // slide only
    double ndy;        // slide only
    int duration_ms;   // slide only
    int32_t pointer_id; // unique pointer id, 0 while unassigned
    bool down;         // single-key pressed state
    bool active;       // combo active state
    bool is_mouse;     // MOUSE_RIGHT / MOUSE_LEFT_LOCKED entry
};

struct sc_file_stamp {
    int64_t mtime_ns; // nanoseconds since epoch (or seconds*1e9 fallback)
    int64_t size;
    bool valid;
};

struct sc_mapping_manager {
    // Fixed table: no realloc, indices stay valid for the manager lifetime.
    struct sc_mapping_entry entries[SC_MAPPING_MAX_ENTRIES];
    int count; // number of occupied slots

    int mouse_right_idx;       // -1 if the template has no such entry
    int mouse_left_locked_idx; // -1 if the template has no such entry

    // Global state
    bool enabled;      // default false (a template must opt in)
    char *active_name; // current active template name (without .json)
    bool dirty;        // entries changed since the last successful save

    // Shortcuts (SDL keycodes)
    SDL_Keycode sc_switch;  // quick template switch (F2)
    SDL_Keycode sc_design;  // design mode (F3)
    SDL_Keycode sc_toggle;  // toggle mapping (F4)
    SDL_Keycode sc_manager; // built-in manager panel (F5)

    // File paths
    char *templates_dir; // .../templates/
    char *config_path;   // .../scrcpy_mapping_config.json

    // Hot-reload tracking (mtime + size, so sub-second edits are detected)
    struct sc_file_stamp active_stamp;
    struct sc_file_stamp config_stamp;

    // Pointer id allocator: monotonic decreasing, occupancy table, never
    // reuses an id that is still held. Avoids -1/-2/-3 (reserved by scrcpy).
    bool pid_used[SC_MAPPING_MAX_ENTRIES];
    int32_t pid_cursor;

    // Debounced template switch (mouse hover must not do file I/O)
    char *pending_switch;
    sc_tick pending_since;

    // Design mode: stage 0 = nothing, 1 = start point, 2 = start+end
    int design_stage;
    double design_nx;
    double design_ny;
    double design_dx;
    double design_dy;
    int design_duration_ms;
    int design_is_slide; // 0 = single-point touch, 1 = slide

    struct sc_mapping_load_stats last_load;

    // Called before every release-all (hot-reload, template switch, disable,
    // destroy): the owner must inject ACTION_UP for every held entry and stop
    // running slides, so that no finger stays stuck on the device.
    void (*release_hook)(void *userdata);
    void *release_opaque;
};

// ---- Lifecycle ----

bool
sc_mapping_manager_init(struct sc_mapping_manager *mm);

// Periodic work: debounced template switch + hot-reload. Safe to call from the
// event loop at a high rate.
void
sc_mapping_manager_tick(struct sc_mapping_manager *mm);

// Check for external file changes and reload if necessary.
void
sc_mapping_manager_reload_if_changed(struct sc_mapping_manager *mm);

// Release every held entry (through the release hook) and free resources.
void
sc_mapping_manager_destroy(struct sc_mapping_manager *mm);

// Register the release hook (must be set before any load/destroy).
void
sc_mapping_manager_set_release_hook(struct sc_mapping_manager *mm,
                                    void (*cb)(void *userdata),
                                    void *userdata);

// ---- Templates ----

// Switch the active template now (load first, then persist; on failure nothing
// changes). Returns false if the template could not be loaded.
bool
sc_mapping_manager_switch(struct sc_mapping_manager *mm, const char *name);

// Request a debounced switch (used by mouse hover: no file I/O here).
void
sc_mapping_manager_request_switch(struct sc_mapping_manager *mm,
                                  const char *name);

void
sc_mapping_manager_set_enabled(struct sc_mapping_manager *mm, bool enabled);

// List template names (without .json). Writes up to max pointers; caller frees
// each string. Returns the total number found.
int
sc_mapping_manager_list(struct sc_mapping_manager *mm, char **names, int max);

// ---- Entries ----

struct sc_mapping_entry *
sc_mapping_manager_find_single(struct sc_mapping_manager *mm, SDL_Keycode kc);

struct sc_mapping_entry *
sc_mapping_manager_find_by_name(struct sc_mapping_manager *mm,
                                const char *key_name);

bool
sc_mapping_manager_key_in_combo(struct sc_mapping_manager *mm, SDL_Keycode kc);

int
sc_mapping_manager_entry_count(struct sc_mapping_manager *mm);

// Add or update an entry (validated with the exact same rules as the JSON
// loader). Marks the template dirty on success.
bool
sc_mapping_manager_upsert(struct sc_mapping_manager *mm, const char *key_name,
                          enum sc_mapping_entry_type type, double nx, double ny,
                          double ndx, double ndy, int duration_ms,
                          char *err, size_t errlen);

bool
sc_mapping_manager_remove_entry(struct sc_mapping_manager *mm,
                                const char *key_name);

// Save the active template if (and only if) it is dirty. Atomic replace.
bool
sc_mapping_manager_save_active(struct sc_mapping_manager *mm);

// Force a save even if not dirty (used by explicit "save" UI actions).
bool
sc_mapping_manager_save_active_force(struct sc_mapping_manager *mm);

// ---- Design mode ----

void
sc_mapping_manager_design_reset(struct sc_mapping_manager *mm);

// Record a clicked point (stage 1 = start, stage 2 = end of a slide).
void
sc_mapping_manager_design_click(struct sc_mapping_manager *mm,
                                double nx, double ny);

bool
sc_mapping_manager_design_has_point(struct sc_mapping_manager *mm);

// Bind the recorded point(s) to the given key name (touch when one point,
// slide when both points were recorded).
bool
sc_mapping_manager_design_bind(struct sc_mapping_manager *mm,
                               const char *key_name);

// ---- Key name helpers ----

SDL_Keycode
sc_mapping_parse_key_name(const char *name);

const char *
sc_mapping_key_name(SDL_Keycode kc);

// Validate a key expression ("a", "W+A", "MOUSE_RIGHT") with the same grammar
// as the loader. On failure, writes a reason into err.
bool
sc_mapping_validate_key_expr(const char *name, char *err, size_t errlen);

char *
sc_mapping_get_base_dir(void);

#endif
