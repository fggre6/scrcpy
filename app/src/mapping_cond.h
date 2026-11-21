#ifndef SC_MAPPING_COND_H
#define SC_MAPPING_COND_H

#include "common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <SDL3/SDL_keycode.h>

// ============================================================
// Dependency conditions ("depends_on")
// ============================================================
//
// A dependency condition reads one *source* and compares its state with the
// required state. Three kinds of source exist:
//
//   SC_COND_KIND_KEY    a physical key or a mouse button (held / released)
//   SC_COND_KIND_ENTRY  a mapping entry, addressed by its key name; the state
//                       is the entry's *logical* state (pressed / toggled on)
//   SC_COND_KIND_FLAG   a built-in feature flag registered in the flag
//                       registry (on / off)
//
// The flag registry is the extension point: a feature (view mode, design
// mode, any feature added later) only has to register a named boolean getter
// to become usable as a dependency source. The template parser resolves the
// name at load time and the runtime evaluator calls the getter.
//
// A reference is written in a template as:
//   "lshift"            -> key condition (implicit "key:" namespace)
//   "A+B"               -> key condition on a combo (all members held)
//   "MOUSE_RIGHT"       -> key condition on a mouse button
//   "key:lshift+A"      -> explicit key condition
//   "entry:VIRT"        -> condition on the entry named "VIRT"
//   "flag:look_mode"    -> condition on a registered feature flag

// Maximum number of members of a key combo condition.
#define SC_COND_MAX_KEYS 4
// Maximum number of conditions attached to a single mapping entry. All of
// them must hold for the entry to be active (logical AND).
#define SC_COND_MAX_DEPS 4
// Maximum length of an entry / flag source name (without the namespace).
#define SC_COND_NAME_MAX 48
// Maximum length of the human readable description of a feature flag.
#define SC_COND_DESC_MAX 96

// The kind of source a condition reads.
enum sc_mapping_cond_kind {
    SC_COND_KIND_KEY = 0,   // physical key or mouse button
    SC_COND_KIND_ENTRY = 1, // mapping entry (logical state)
    SC_COND_KIND_FLAG = 2,  // built-in feature flag
};

// The state the source must be in.
enum sc_mapping_cond_state {
    SC_COND_STATE_ON = 0,  // held / active / on
    SC_COND_STATE_OFF = 1, // released / inactive / off
};

// One dependency condition.
struct sc_mapping_cond {
    enum sc_mapping_cond_kind kind;
    enum sc_mapping_cond_state state;
    // SC_COND_KIND_KEY: the key members. nkeys == 1 is a single key, nkeys > 1
    // is a combo: every member must be held for the condition to hold.
    SDL_Keycode keys[SC_COND_MAX_KEYS];
    int nkeys;
    // SC_COND_KIND_ENTRY / SC_COND_KIND_FLAG: the source name.
    char name[SC_COND_NAME_MAX];
};

// ============================================================
// Feature flag registry
// ============================================================

// Getter of a feature flag. The context is the pointer installed with
// sc_cond_flag_set_ctx() (the input manager in the application).
typedef bool (*sc_cond_flag_fn)(void *ctx);

struct sc_cond_flag_def {
    char name[SC_COND_NAME_MAX];
    char desc[SC_COND_DESC_MAX];
    sc_cond_flag_fn fn;
};

// Register a named boolean source. Returns false when the name is empty,
// already used or the table is full.
bool
sc_cond_flag_register(const char *name, const char *desc, sc_cond_flag_fn fn);

// Look up a registered flag (NULL when unknown).
const struct sc_cond_flag_def *
sc_cond_flag_find(const char *name);

// Number of registered flags / flag at a given index (NULL when out of range).
int
sc_cond_flag_count(void);

const struct sc_cond_flag_def *
sc_cond_flag_at(int index);

// True when at least one flag has been registered. The template parser only
// validates "flag:" names when the registry is up, so that a template can be
// parsed (and a test can run) before the application registered its flags.
bool
sc_cond_flag_registry_ready(void);

// Install the context passed to every flag getter.
void
sc_cond_flag_set_ctx(void *ctx);

void *
sc_cond_flag_ctx(void);

// ============================================================
// Parsing / rendering
// ============================================================

// Parse a key expression: a single key, a mouse button, or a combo
// ("A+B+C"). Returns false and fills err on failure.
bool
sc_cond_parse_key_expr(const char *name, SDL_Keycode *out, int *nkeys_out,
                       char *err, size_t errlen);

// Parse a single dependency reference (see the grammar above) into a
// condition. The state of the condition is set to SC_COND_STATE_ON.
bool
sc_cond_parse_ref(const char *expr, struct sc_mapping_cond *out, char *err,
                  size_t errlen);

// Parse a "depends_state" value. Accepts "pressed" / "released" / "active" /
// "inactive" / "on" / "off" / "enabled" / "disabled" (case insensitive).
bool
sc_cond_parse_state(const char *text, enum sc_mapping_cond_state *out, char *err,
                    size_t errlen);

// Render a condition back to its canonical template form ("lshift+A",
// "entry:VIRT", "flag:look_mode"). Writes into buf and returns it.
const char *
sc_cond_to_name(const struct sc_mapping_cond *c, char *buf, size_t buflen);

// Canonical state word of a condition ("pressed" / "released" for keys,
// "active" / "inactive" for entries, "on" / "off" for flags).
const char *
sc_cond_state_name(const struct sc_mapping_cond *c);

// Build a single-key dependency condition (helper for the manager panel).
void
sc_cond_init_key(struct sc_mapping_cond *c, SDL_Keycode kc,
                 enum sc_mapping_cond_state state);

#endif
