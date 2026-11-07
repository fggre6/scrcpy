#ifndef SC_OVERLAY_UI_H
#define SC_OVERLAY_UI_H

#include "common.h"

#include <stdbool.h>
#include <SDL3/SDL.h>
#include "util/tick.h"

struct sc_mapping_manager;

// One overlay at a time: every mode shares the same drawing and input
// dispatch, so two overlays can never answer the same event.
enum sc_overlay_mode {
    SC_OVERLAY_NONE = 0,
    SC_OVERLAY_TEMPLATE_PICKER = 1, // F2: quick template switch
    SC_OVERLAY_DESIGN = 2,          // F3: bind one key to one point
    SC_OVERLAY_MANAGER = 3,         // F5: built-in editor
};

#define SC_MANAGER_EDIT_FIELDS 5 // x, y, dx, dy, duration_ms

struct sc_overlay {
    enum sc_overlay_mode mode;
    struct sc_mapping_manager *mm; // for rendering the manager panel

    // Template switch popup (F2)
    char **names;
    int count;
    int hovered;          // index under cursor, -1 if none
    SDL_FRect popup_rect; // overall popup bounds (window coords)

    // Design mode banner (F3)
    char design_hint[256];

    // Manager panel (F5)
    char **mgr_names;     // template list snapshot
    int mgr_count;
    int mgr_focus;        // 0 = template list, 1 = entry list
    int mgr_tmpl_sel;
    int mgr_entry_sel;
    int mgr_edit_field;   // -1 = not editing, else index into the detail
    char mgr_edit_buf[24];
    int mgr_edit_len;
    bool mgr_capture_key; // next key press becomes the new entry's key
    int mgr_pick;         // 0 = none, 1 = pick start, 2 = pick end point
    char mgr_hint[256];

    // Toggle indicator (brief flash)
    bool show_toggle;
    bool mapping_enabled;
    sc_tick toggle_until;

    // status line (bottom of window)
    char status[256];
    sc_tick status_until;
};

void
sc_overlay_init(struct sc_overlay *ov);

void
sc_overlay_destroy(struct sc_overlay *ov);

// ---- F2: template picker ----

// Show the template switch popup with the given names.
// Takes ownership of the names array (will free on hide).
void
sc_overlay_show_switch(struct sc_overlay *ov, char **names, int count);

void
sc_overlay_hide_switch(struct sc_overlay *ov);

// Update hover state from mouse position. Returns the hovered index or -1.
int
sc_overlay_update_switch_mouse(struct sc_overlay *ov, float mx, float my);

// ---- F3: design mode ----

void
sc_overlay_set_design(struct sc_overlay *ov, bool active, const char *hint);

// ---- F5: built-in manager panel ----

// Open/close the manager panel. Takes ownership of the template name list.
void
sc_overlay_set_manager(struct sc_overlay *ov, bool open,
                       struct sc_mapping_manager *mm,
                       char **names, int count);

// Click inside the manager panel. Returns true when the click was consumed.
bool
sc_overlay_manager_mouse(struct sc_overlay *ov, float mx, float my,
                         int win_w, int win_h);

// Set the manager hint line (errors/status of the editor).
void
sc_overlay_manager_set_hint(struct sc_overlay *ov, const char *hint);

// ---- shared ----

void
sc_overlay_show_toggle(struct sc_overlay *ov, bool enabled);

void
sc_overlay_set_status(struct sc_overlay *ov, const char *msg);

// Render all overlay elements on top of the frame.
void
sc_overlay_render(struct sc_overlay *ov, SDL_Renderer *renderer,
                  int win_w, int win_h);

#endif
