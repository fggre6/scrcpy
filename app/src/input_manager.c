#include "input_manager.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <SDL3/SDL.h>

#include "android/input.h"
#include "android/keycodes.h"
#include "events.h"
#include "input_events.h"
#include "mapping_manager.h"
#include "overlay_ui.h"
#include "screen.h"
#include "shortcut_mod.h"
#include "util/log.h"
#include "util/sdl.h"

// ==================== 辅助函数 ====================

static struct sc_mapping_manager *
mm_of(struct sc_input_manager *im) {
    return &im->screen->mm;
}

static struct sc_overlay *
ov_of(struct sc_input_manager *im) {
    return &im->screen->overlay;
}

// Send a touch at a normalized position (nx, ny in [0,1])
// Positions are converted with the *current* frame_size on every injection,
// so rotations / --max-size / --crop keep working (the server's PositionMapper
// compares the sent screen size against the current video size).
static bool
inject_touch_norm_once(struct sc_input_manager *im,
                       enum android_motionevent_action action,
                       double nx, double ny, int32_t pointer_id) {
    if (!im->controller) {
        return true; // teardown: nothing to inject
    }
    struct sc_screen *screen = im->screen;
    int32_t x = (int32_t)(nx * screen->frame_size.width);
    int32_t y = (int32_t)(ny * screen->frame_size.height);
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    bool up = action == AMOTION_EVENT_ACTION_UP;

    struct sc_control_msg msg;
    msg.type = SC_CONTROL_MSG_TYPE_INJECT_TOUCH_EVENT;
    msg.inject_touch_event.action = action;
    msg.inject_touch_event.position.screen_size = screen->frame_size;
    msg.inject_touch_event.position.point.x = x;
    msg.inject_touch_event.position.point.y = y;
    msg.inject_touch_event.pointer_id = pointer_id;
    msg.inject_touch_event.pressure = up ? 0.0f : 1.0f;
    msg.inject_touch_event.action_button = 0;
    msg.inject_touch_event.buttons = 0;

    return sc_controller_push_msg(im->controller, &msg);
}

// ACTION_DOWN / ACTION_UP must never be lost: the server allocates a pointer
// slot per id and only frees it on UP, so a dropped UP leaks a slot forever
// (MAX_POINTERS = 10, then every touch is refused). MOVE is droppable, DOWN and
// UP are retried.
static bool
inject_touch_norm(struct sc_input_manager *im,
                  enum android_motionevent_action action,
                  double nx, double ny, int32_t pointer_id) {
    bool critical = action == AMOTION_EVENT_ACTION_DOWN
                 || action == AMOTION_EVENT_ACTION_UP;
    int attempts = critical ? 3 : 1;
    for (int i = 0; i < attempts; i++) {
        if (inject_touch_norm_once(im, action, nx, ny, pointer_id)) {
            return true;
        }
        if (i + 1 < attempts) {
            SDL_Delay(5);
        }
    }
    if (critical) {
        LOGE("Could not inject %s for pointer %d (3 attempts)",
             action == AMOTION_EVENT_ACTION_UP ? "UP" : "DOWN",
             (int) pointer_id);
    }
    return false;
}

// Inject a touch for a mapping entry (uses its normalized coords)
static bool
inject_entry(struct sc_input_manager *im, struct sc_mapping_entry *e,
             enum android_motionevent_action action) {
    return inject_touch_norm(im, action, e->nx, e->ny, e->pointer_id);
}

// ==================== 滑动（hold-to-slide） ====================

// Rate limits: a slide must not be able to flood the control queue, and a
// 0-delay configuration is meaningless (4 ms is the floor).
#define SLIDE_STEPS_DEFAULT 20
#define SLIDE_STEPS_MIN 2
#define SLIDE_STEPS_MAX 120
#define SLIDE_STEP_MIN_MS 4
#define SLIDE_DURATION_MIN_MS 50
#define SLIDE_DURATION_MAX_MS 5000

static void
slide_stop_all(struct sc_input_manager *im);

static bool
slide_start(struct sc_input_manager *im, struct sc_mapping_entry *e);

static void
slide_stop_for_entry(struct sc_input_manager *im, struct sc_mapping_entry *e);

static double
clamp01(double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

static bool
slide_start(struct sc_input_manager *im, struct sc_mapping_entry *e) {
    int slot = -1;
    for (int i = 0; i < SC_MAX_SLIDES; i++) {
        if (!im->slides[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        LOGW("Too many concurrent slides (max %d), '%s' ignored",
             SC_MAX_SLIDES, e->key_name);
        return false;
    }

    int duration_ms = e->duration_ms > 0 ? e->duration_ms : 200;
    if (duration_ms < SLIDE_DURATION_MIN_MS) {
        LOGW("Slide '%s': duration_ms %d < %d, clamped", e->key_name,
             duration_ms, SLIDE_DURATION_MIN_MS);
        duration_ms = SLIDE_DURATION_MIN_MS;
    } else if (duration_ms > SLIDE_DURATION_MAX_MS) {
        LOGW("Slide '%s': duration_ms %d > %d, clamped", e->key_name,
             duration_ms, SLIDE_DURATION_MAX_MS);
        duration_ms = SLIDE_DURATION_MAX_MS;
    }

    int steps = SLIDE_STEPS_DEFAULT;
    int step_ms = duration_ms / steps;
    if (step_ms < SLIDE_STEP_MIN_MS) {
        LOGW("Slide '%s': %d steps in %d ms needs <%d ms per step, "
             "using %d ms", e->key_name, steps, duration_ms,
             SLIDE_STEP_MIN_MS, SLIDE_STEP_MIN_MS);
        step_ms = SLIDE_STEP_MIN_MS;
        steps = duration_ms / step_ms;
    }
    if (steps < SLIDE_STEPS_MIN) steps = SLIDE_STEPS_MIN;
    if (steps > SLIDE_STEPS_MAX) steps = SLIDE_STEPS_MAX;

    struct sc_slide_run *r = &im->slides[slot];
    // Value copy only: a later reload may replace the entry (and the whole
    // entry table), the run must not reference it.
    r->active = true;
    r->pointer_id = e->pointer_id;
    r->nx = e->nx;
    r->ny = e->ny;
    r->ndx = e->ndx;
    r->ndy = e->ndy;
    r->cur_nx = e->nx;
    r->cur_ny = e->ny;
    r->steps = steps;
    r->step_ms = step_ms;
    r->step = 0;
    r->start = sc_tick_now();

    if (!inject_touch_norm(im, AMOTION_EVENT_ACTION_DOWN, r->nx, r->ny,
                           r->pointer_id)) {
        r->active = false;
        return false;
    }
    return true;
}

static void
slide_stop(struct sc_input_manager *im, struct sc_slide_run *r) {
    if (!r->active) return;
    // The segment is closed with an UP at the current point. A single MOVE
    // jumping back to the start would be seen by the device as a very fast
    // stroke in the opposite direction (a "fling"), so the next segment is
    // opened with a fresh DOWN at the start point instead.
    if (!inject_touch_norm(im, AMOTION_EVENT_ACTION_UP, r->cur_nx, r->cur_ny,
                           r->pointer_id)) {
        LOGE("Slide UP could not be injected for pointer %d",
             (int) r->pointer_id);
    }
    r->active = false;
}

static void
slide_stop_for_entry(struct sc_input_manager *im, struct sc_mapping_entry *e) {
    for (int i = 0; i < SC_MAX_SLIDES; i++) {
        struct sc_slide_run *r = &im->slides[i];
        if (r->active && r->pointer_id == e->pointer_id) {
            slide_stop(im, r);
        }
    }
}

static void
slide_stop_all(struct sc_input_manager *im) {
    for (int i = 0; i < SC_MAX_SLIDES; i++) {
        if (im->slides[i].active) {
            slide_stop(im, &im->slides[i]);
        }
    }
}

// Advance running slides from the event loop. No thread is detached: the
// progression is driven by sc_tick_now(), so there is nothing to join and
// nothing can outlive the controller.
void
sc_input_manager_tick(struct sc_input_manager *im) {
    bool any = false;
    for (int i = 0; i < SC_MAX_SLIDES; i++) {
        if (im->slides[i].active) {
            any = true;
            break;
        }
    }
    if (!any) return; // cheap fast path: tick runs on every event

    struct sc_mapping_manager *mm = mm_of(im);
    // Any of these invalidates the slide: stop it and lift the finger.
    bool blocked = im->screen->paused || im->disconnected || !im->controller
                || !mm->enabled;

    for (int i = 0; i < SC_MAX_SLIDES; i++) {
        struct sc_slide_run *r = &im->slides[i];
        if (!r->active) continue;
        if (blocked) {
            slide_stop(im, r);
            continue;
        }

        sc_tick elapsed = sc_tick_now() - r->start;
        int desired = (int) (elapsed / SC_TICK_FROM_MS(r->step_ms));
        if (desired > r->steps) desired = r->steps;
        if (desired <= r->step) continue;

        // Only the newest position is sent (intermediate steps are skipped
        // rather than queued), which keeps the control queue out of trouble.
        double t = (double) desired / (double) r->steps;
        r->cur_nx = clamp01(r->nx + r->ndx * t);
        r->cur_ny = clamp01(r->ny + r->ndy * t);
        r->step = desired;
        inject_touch_norm(im, AMOTION_EVENT_ACTION_MOVE, r->cur_nx, r->cur_ny,
                          r->pointer_id);

        if (desired >= r->steps) {
            // Segment finished. Close it with UP at the end point and reopen
            // at the start with a DOWN (holding the key keeps sliding).
            slide_stop(im, r);

            struct sc_mapping_entry *e = NULL;
            for (int k = 0; k < SC_MAPPING_MAX_ENTRIES; k++) {
                if (mm->entries[k].key_name && mm->entries[k].down
                        && mm->entries[k].pointer_id == r->pointer_id) {
                    e = &mm->entries[k];
                    break;
                }
            }
            if (e) {
                slide_start(im, e);
            }
        }
    }
}

// Lift every held entry (combos and single keys). Called before any state that
// invalidates the entries (hot-reload, template switch, disable, destroy).
static void
mapping_release_all(void *userdata) {
    struct sc_input_manager *im = userdata;
    struct sc_mapping_manager *mm = mm_of(im);

    // Running slides must be stopped (and their UP injected) before the
    // entries they were derived from are replaced.
    slide_stop_all(im);

    for (int i = 0; i < SC_MAPPING_MAX_ENTRIES; i++) {
        struct sc_mapping_entry *e = &mm->entries[i];
        if (!e->key_name) continue;
        if (e->active) {
            e->active = false;
            inject_entry(im, e, AMOTION_EVENT_ACTION_UP);
        }
        if (e->down) {
            e->down = false;
            inject_entry(im, e, AMOTION_EVENT_ACTION_UP);
        }
    }
    memset(im->g_pressed, 0, sizeof(im->g_pressed));
    im->left_click_locked_down = false;
    im->right_click_down = false;
}

// Update all combos based on currently pressed keys
static void
update_combos(struct sc_input_manager *im) {
    struct sc_mapping_manager *mm = mm_of(im);
    for (int i = 0; i < SC_MAPPING_MAX_ENTRIES; ++i) {
        struct sc_mapping_entry *c = &mm->entries[i];
        if (!c->key_name || c->nkeys < 2) continue;

        bool match = true;
        for (int j = 0; j < c->nkeys; ++j) {
            if (!im->g_pressed[c->keys[j] & 0xFF]) {
                match = false;
                break;
            }
        }

        if (match && !c->active) {
            // Release single-key downs for combo members
            for (int j = 0; j < c->nkeys; ++j) {
                struct sc_mapping_entry *m =
                    sc_mapping_manager_find_single(mm, c->keys[j]);
                if (m && m->down) {
                    inject_entry(im, m, AMOTION_EVENT_ACTION_UP);
                    m->down = false;
                }
            }
            c->active = true;
            inject_entry(im, c, AMOTION_EVENT_ACTION_DOWN);
            LOGI("Combo down: %s -> (%.3f, %.3f)", c->key_name, c->nx, c->ny);
        } else if (!match && c->active) {
            c->active = false;
            inject_entry(im, c, AMOTION_EVENT_ACTION_UP);
            LOGI("Combo up: %s", c->key_name);
            // Re-press single keys still held
            for (int j = 0; j < c->nkeys; ++j) {
                if (!im->g_pressed[c->keys[j] & 0xFF]) continue;
                struct sc_mapping_entry *m =
                    sc_mapping_manager_find_single(mm, c->keys[j]);
                // Slides are never combo members (rejected at load time)
                if (m && !m->down && m->type != SC_MAPPING_TYPE_SLIDE) {
                    inject_entry(im, m, AMOTION_EVENT_ACTION_DOWN);
                    m->down = true;
                }
            }
        }
    }
}

// ==================== 模板切换弹窗 ====================

static void
open_template_switch(struct sc_input_manager *im) {
    struct sc_mapping_manager *mm = mm_of(im);
    struct sc_overlay *ov = ov_of(im);

    char **names = NULL;
    int total = sc_mapping_manager_list(mm, NULL, 0);
    if (total <= 0) {
        sc_overlay_set_status(ov, "No templates found");
        return;
    }
    names = calloc((size_t)total, sizeof(char *));
    if (!names) return;
    sc_mapping_manager_list(mm, names, total);

    sc_overlay_show_switch(ov, names, total);
    im->switch_open = true;
}

static void
close_template_switch(struct sc_input_manager *im) {
    if (im->switch_open) {
        sc_overlay_hide_switch(ov_of(im));
        im->switch_open = false;
    }
}

// ==================== 设计模式 ====================

static void
toggle_design_mode(struct sc_input_manager *im) {
    struct sc_overlay *ov = ov_of(im);
    im->design_mode = !im->design_mode;
    if (im->design_mode) {
        sc_mapping_manager_design_reset(mm_of(im));
        sc_overlay_set_design(ov, true,
            "Design: click position, then press a key. F3 to exit.");
        LOGI("Design mode ON");
    } else {
        // Save only if something actually changed (dirty flag), never blindly
        sc_mapping_manager_design_reset(mm_of(im));
        if (sc_mapping_manager_save_active(mm_of(im))) {
            sc_overlay_set_status(ov, "Design mode off");
        }
        sc_overlay_set_design(ov, false, NULL);
        LOGI("Design mode OFF");
    }
}

// ==================== 内置管理面板（F5） ====================

// The panel edits the live entry table through the same validation functions
// used by the JSON loader (sc_mapping_manager_upsert /
// sc_mapping_validate_key_expr), so the editor and the loader cannot disagree.

static int
entry_index_by_sel(struct sc_mapping_manager *mm, int sel) {
    int n = 0;
    for (int i = 0; i < SC_MAPPING_MAX_ENTRIES; i++) {
        if (!mm->entries[i].key_name) continue;
        if (n == sel) return i;
        n++;
    }
    return -1;
}

static struct sc_mapping_entry *
manager_selected_entry(struct sc_input_manager *im) {
    struct sc_overlay *ov = ov_of(im);
    int idx = entry_index_by_sel(mm_of(im), ov->mgr_entry_sel);
    return idx >= 0 ? &mm_of(im)->entries[idx] : NULL;
}

static void
manager_refresh_templates(struct sc_input_manager *im) {
    struct sc_overlay *ov = ov_of(im);
    struct sc_mapping_manager *mm = mm_of(im);
    int total = sc_mapping_manager_list(mm, NULL, 0);
    char **names = NULL;
    if (total > 0) {
        names = calloc((size_t) total, sizeof(char *));
        if (names) {
            sc_mapping_manager_list(mm, names, total);
        } else {
            total = 0;
        }
    }
    free(ov->mgr_names);
    ov->mgr_names = names;
    ov->mgr_count = names ? total : 0;
    if (ov->mgr_tmpl_sel >= ov->mgr_count) {
        ov->mgr_tmpl_sel = ov->mgr_count > 0 ? ov->mgr_count - 1 : 0;
    }
}

static void
manager_open(struct sc_input_manager *im) {
    struct sc_overlay *ov = ov_of(im);
    struct sc_mapping_manager *mm = mm_of(im);

    // F3 and F5 are mutually exclusive
    if (im->design_mode) {
        im->design_mode = false;
        sc_overlay_set_design(ov, false, NULL);
    }
    if (im->switch_open) {
        close_template_switch(im);
    }
    // Entering the panel restores all input state (no finger left down)
    mapping_release_all(im);

    sc_overlay_set_manager(ov, true, mm, NULL, 0);
    manager_refresh_templates(im);

    ov->mgr_focus = 1;
    ov->mgr_entry_sel = 0;
    for (int i = 0; i < ov->mgr_count; i++) {
        if (mm->active_name && ov->mgr_names[i]
                && strcmp(mm->active_name, ov->mgr_names[i]) == 0) {
            ov->mgr_tmpl_sel = i;
            break;
        }
    }
    sc_overlay_manager_set_hint(ov,
        "1-5 edit field, Enter commit, N then key = new entry");
    LOGI("Manager panel opened");
}

static void
manager_close(struct sc_input_manager *im) {
    struct sc_overlay *ov = ov_of(im);
    struct sc_mapping_manager *mm = mm_of(im);

    sc_overlay_set_manager(ov, false, NULL, NULL, 0);
    // Restore every bit of input state the panel may have touched
    mapping_release_all(im);
    im->left_click_locked_down = false;
    im->right_click_down = false;
    // Dirty-gated: nothing is written when nothing was modified
    sc_mapping_manager_save_active(mm);
    LOGI("Manager panel closed");
}

static void
manager_begin_edit(struct sc_input_manager *im, int field) {
    struct sc_overlay *ov = ov_of(im);
    struct sc_mapping_entry *e = manager_selected_entry(im);
    if (!e) {
        sc_overlay_manager_set_hint(ov, "No entry selected");
        return;
    }
    if (field >= 2 && field <= 4 && e->type != SC_MAPPING_TYPE_SLIDE) {
        sc_overlay_manager_set_hint(ov,
            "dx/dy/duration need a slide entry (press T)");
        return;
    }
    ov->mgr_edit_field = field;
    ov->mgr_edit_len = 0;
    ov->mgr_edit_buf[0] = '\0';
    if (field == 0) snprintf(ov->mgr_edit_buf, sizeof(ov->mgr_edit_buf),
                             "%.5f", e->nx);
    else if (field == 1) snprintf(ov->mgr_edit_buf, sizeof(ov->mgr_edit_buf),
                                  "%.5f", e->ny);
    else if (field == 2) snprintf(ov->mgr_edit_buf, sizeof(ov->mgr_edit_buf),
                                  "%.5f", e->ndx);
    else if (field == 3) snprintf(ov->mgr_edit_buf, sizeof(ov->mgr_edit_buf),
                                  "%.5f", e->ndy);
    else if (field == 4) snprintf(ov->mgr_edit_buf, sizeof(ov->mgr_edit_buf),
                                  "%d", e->duration_ms);
    ov->mgr_edit_len = (int) strlen(ov->mgr_edit_buf);
}

static void
manager_commit_edit(struct sc_input_manager *im) {
    struct sc_overlay *ov = ov_of(im);
    struct sc_mapping_manager *mm = mm_of(im);
    struct sc_mapping_entry *e = manager_selected_entry(im);
    int field = ov->mgr_edit_field;
    ov->mgr_edit_field = -1;
    if (!e || field < 0) return;

    double v = atof(ov->mgr_edit_buf);
    double nx = e->nx, ny = e->ny, ndx = e->ndx, ndy = e->ndy;
    int dur = e->duration_ms;
    if (field == 0) nx = v;
    else if (field == 1) ny = v;
    else if (field == 2) ndx = v;
    else if (field == 3) ndy = v;
    else if (field == 4) dur = (int) v;

    char err[128];
    if (!sc_mapping_manager_upsert(mm, e->key_name, e->type, nx, ny, ndx, ndy,
                                   dur, err, sizeof(err))) {
        char msg[160];
        snprintf(msg, sizeof(msg), "Rejected: %s", err);
        sc_overlay_manager_set_hint(ov, msg);
        LOGW("Manager edit rejected: %s", err);
    } else {
        sc_overlay_manager_set_hint(ov, "Updated (not saved yet: press S)");
    }
}

static void
manager_create_entry(struct sc_input_manager *im, const char *key_name) {
    struct sc_overlay *ov = ov_of(im);
    struct sc_mapping_manager *mm = mm_of(im);

    // Use the last picked point if any, else the frame center
    double nx = 0.5, ny = 0.5;
    if (sc_mapping_manager_design_has_point(mm)) {
        nx = mm->design_nx;
        ny = mm->design_ny;
    }
    char err[128];
    if (!sc_mapping_manager_upsert(mm, key_name, SC_MAPPING_TYPE_TOUCH,
                                   nx, ny, 0.0, 0.0, 0, err, sizeof(err))) {
        char msg[160];
        snprintf(msg, sizeof(msg), "Rejected: %s", err);
        sc_overlay_manager_set_hint(ov, msg);
        LOGW("Manager new entry rejected: %s", err);
        return;
    }
    sc_mapping_manager_design_reset(mm);
    manager_refresh_templates(im);
    sc_overlay_manager_set_hint(ov, "Entry added (press S to save)");
}

static void
manager_remove_selected(struct sc_input_manager *im) {
    struct sc_overlay *ov = ov_of(im);
    struct sc_mapping_entry *e = manager_selected_entry(im);
    if (!e) {
        sc_overlay_manager_set_hint(ov, "No entry selected");
        return;
    }
    char name[64];
    snprintf(name, sizeof(name), "%s", e->key_name);
    if (sc_mapping_manager_remove_entry(mm_of(im), name)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Removed %s (press S to save)", name);
        sc_overlay_manager_set_hint(ov, msg);
        if (ov->mgr_entry_sel > 0) ov->mgr_entry_sel--;
    }
}

static void
manager_toggle_type(struct sc_input_manager *im) {
    struct sc_overlay *ov = ov_of(im);
    struct sc_mapping_entry *e = manager_selected_entry(im);
    if (!e) {
        sc_overlay_manager_set_hint(ov, "No entry selected");
        return;
    }
    if (e->is_mouse) {
        sc_overlay_manager_set_hint(ov, "Mouse entries cannot be slides");
        return;
    }
    enum sc_mapping_entry_type new_type =
        e->type == SC_MAPPING_TYPE_SLIDE ? SC_MAPPING_TYPE_TOUCH
                                         : SC_MAPPING_TYPE_SLIDE;
    double ndx = e->type == SC_MAPPING_TYPE_SLIDE ? e->ndx : 0.0;
    double ndy = e->type == SC_MAPPING_TYPE_SLIDE ? e->ndy : -0.3;
    int dur = e->type == SC_MAPPING_TYPE_SLIDE ? e->duration_ms : 200;

    char err[128];
    if (!sc_mapping_manager_upsert(mm_of(im), e->key_name, new_type,
                                   e->nx, e->ny, ndx, ndy, dur,
                                   err, sizeof(err))) {
        char msg[160];
        snprintf(msg, sizeof(msg), "Rejected: %s", err);
        sc_overlay_manager_set_hint(ov, msg);
        return;
    }
    sc_overlay_manager_set_hint(ov, new_type == SC_MAPPING_TYPE_SLIDE
        ? "Now a slide (edit 3/4/5)" : "Now a touch entry");
}

static void
manager_pick_point(struct sc_input_manager *im, double nx, double ny) {
    struct sc_overlay *ov = ov_of(im);
    struct sc_mapping_entry *e = manager_selected_entry(im);
    int mode = ov->mgr_pick;
    ov->mgr_pick = 0;

    if (!e) {
        // No entry yet: remember the point for the next "N <key>" creation
        mm_of(im)->design_nx = nx;
        mm_of(im)->design_ny = ny;
        mm_of(im)->design_stage = 1;
        sc_overlay_manager_set_hint(ov, "Point stored: press N then a key");
        return;
    }

    double x = e->nx, y = e->ny, dx = e->ndx, dy = e->ndy;
    if (mode == 1) {
        x = nx;
        y = ny;
    } else {
        // end point of a slide, expressed as a delta from the start
        dx = nx - e->nx;
        dy = ny - e->ny;
    }
    char err[128];
    if (!sc_mapping_manager_upsert(mm_of(im), e->key_name, e->type, x, y,
                                   dx, dy, e->duration_ms, err, sizeof(err))) {
        char msg[160];
        snprintf(msg, sizeof(msg), "Rejected: %s", err);
        sc_overlay_manager_set_hint(ov, msg);
        return;
    }
    sc_overlay_manager_set_hint(ov, mode == 1 ? "Point updated"
                                              : "Slide end point updated");
}

// Returns true when the key was consumed by the panel.
static bool
manager_handle_key(struct sc_input_manager *im, SDL_Keycode kc) {
    struct sc_overlay *ov = ov_of(im);
    struct sc_mapping_manager *mm = mm_of(im);

    if (ov->mgr_capture_key) {
        ov->mgr_capture_key = false;
        if (kc == SDLK_ESCAPE) {
            sc_overlay_manager_set_hint(ov, "Cancelled");
        } else {
            manager_create_entry(im, sc_mapping_key_name(kc));
        }
        return true;
    }

    if (ov->mgr_edit_field >= 0) {
        if (kc == SDLK_ESCAPE) {
            ov->mgr_edit_field = -1;
            sc_overlay_manager_set_hint(ov, "Edit cancelled");
        } else if (kc == SDLK_RETURN || kc == SDLK_KP_ENTER) {
            manager_commit_edit(im);
        } else if (kc == SDLK_BACKSPACE) {
            if (ov->mgr_edit_len > 0) {
                ov->mgr_edit_buf[--ov->mgr_edit_len] = '\0';
            }
        } else {
            char ch = 0;
            if (kc >= SDLK_0 && kc <= SDLK_9) {
                ch = (char) ('0' + (kc - SDLK_0));
            } else if (kc == SDLK_PERIOD || kc == SDLK_KP_PERIOD) {
                ch = '.';
            } else if (kc == SDLK_MINUS) {
                ch = '-';
            }
            if (ch && ov->mgr_edit_len < (int) sizeof(ov->mgr_edit_buf) - 1) {
                ov->mgr_edit_buf[ov->mgr_edit_len++] = ch;
                ov->mgr_edit_buf[ov->mgr_edit_len] = '\0';
            }
        }
        return true;
    }

    switch (kc) {
    case SDLK_TAB:
        ov->mgr_focus = ov->mgr_focus == 0 ? 1 : 0;
        return true;
    case SDLK_UP:
        if (ov->mgr_focus == 0) {
            if (ov->mgr_tmpl_sel > 0) ov->mgr_tmpl_sel--;
        } else if (ov->mgr_entry_sel > 0) {
            ov->mgr_entry_sel--;
        }
        return true;
    case SDLK_DOWN:
        if (ov->mgr_focus == 0) {
            if (ov->mgr_tmpl_sel + 1 < ov->mgr_count) ov->mgr_tmpl_sel++;
        } else if (ov->mgr_entry_sel + 1 < mm->count) {
            ov->mgr_entry_sel++;
        }
        return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: {
        if (ov->mgr_focus == 0) {
            if (ov->mgr_names && ov->mgr_tmpl_sel < ov->mgr_count) {
                const char *n = ov->mgr_names[ov->mgr_tmpl_sel];
                if (n && sc_mapping_manager_switch(mm, n)) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "Template: %s", n);
                    sc_overlay_manager_set_hint(ov, msg);
                    ov->mgr_entry_sel = 0;
                } else {
                    sc_overlay_manager_set_hint(ov, "Load failed");
                }
            }
            return true;
        }
        manager_begin_edit(im, 0);
        return true;
    }
    case SDLK_1: manager_begin_edit(im, 0); return true;
    case SDLK_2: manager_begin_edit(im, 1); return true;
    case SDLK_3: manager_begin_edit(im, 2); return true;
    case SDLK_4: manager_begin_edit(im, 3); return true;
    case SDLK_5: manager_begin_edit(im, 4); return true;
    case SDLK_N:
        ov->mgr_capture_key = true;
        sc_overlay_manager_set_hint(ov, "Press the key to bind (Esc cancels)");
        return true;
    case SDLK_DELETE:
    case SDLK_X:
        manager_remove_selected(im);
        return true;
    case SDLK_T:
        manager_toggle_type(im);
        return true;
    case SDLK_P:
        ov->mgr_pick = 1;
        sc_overlay_manager_set_hint(ov, "Click a point on the video");
        return true;
    case SDLK_O:
        ov->mgr_pick = 2;
        sc_overlay_manager_set_hint(ov, "Click the slide end point");
        return true;
    case SDLK_E: {
        bool en = !mm->enabled;
        sc_mapping_manager_set_enabled(mm, en);
        sc_overlay_show_toggle(ov, en);
        sc_overlay_manager_set_hint(ov, en ? "Mapping enabled"
                                           : "Mapping disabled");
        return true;
    }
    case SDLK_S:
        if (sc_mapping_manager_save_active_force(mm)) {
            sc_overlay_manager_set_hint(ov, "Saved");
            sc_overlay_set_status(ov, "Template saved");
        } else {
            sc_overlay_manager_set_hint(ov, "Save failed");
        }
        return true;
    default:
        return true; // panel is modal: never forward keys while it is open
    }
}

static void
manager_handle_click(struct sc_input_manager *im, double win_x, double win_y) {
    struct sc_overlay *ov = ov_of(im);
    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(im->screen->window, &win_w, &win_h);

    if (sc_overlay_manager_mouse(ov, (float) win_x, (float) win_y,
                                 win_w, win_h)) {
        return; // consumed by the panel
    }

    // Click on the video area: only used for coordinate picking
    if (ov->mgr_pick) {
        struct sc_point p = sc_screen_convert_window_to_frame_coords(
            im->screen, (float) win_x, (float) win_y);
        double nx = (double) p.x / im->screen->frame_size.width;
        double ny = (double) p.y / im->screen->frame_size.height;
        if (nx < 0) nx = 0;
        if (nx > 1) nx = 1;
        if (ny < 0) ny = 0;
        if (ny > 1) ny = 1;
        manager_pick_point(im, nx, ny);
    }
}


// ==================== 初始化 ====================

static bool
overlay_manager_open(struct sc_input_manager *im) {
    return ov_of(im)->mode == SC_OVERLAY_MANAGER;
}

void
sc_input_manager_init(struct sc_input_manager *im,
                      const struct sc_input_manager_params *params) {
    assert(!params->kp || params->kp->ops);
    assert(!params->mp || params->mp->ops);
    assert(!params->gp || params->gp->ops);

    im->controller = params->controller;
    im->fp = params->fp;
    im->screen = params->screen;
    im->kp = params->kp;
    im->mp = params->mp;
    im->gp = params->gp;
    im->camera = params->camera;

    im->mouse_bindings = params->mouse_bindings;
    im->legacy_paste = params->legacy_paste;
    im->clipboard_autosync = params->clipboard_autosync;

    im->sdl_shortcut_mods = sc_shortcut_mods_to_sdl(params->shortcut_mods);

    im->vfinger_down = false;
    im->vfinger_invert_x = false;
    im->vfinger_invert_y = false;

    im->mouse_buttons_state = 0;

    im->last_keycode = SDLK_UNKNOWN;
    im->last_mod = 0;
    im->key_repeat = 0;

    im->next_sequence = 1;

    im->disconnected = false;

    // custom mapping state
    memset(im->g_pressed, 0, sizeof(im->g_pressed));
    im->left_click_locked = false;
    im->left_click_locked_down = false;
    im->right_click_down = false;
    im->design_mode = false;
    im->switch_open = false;

    sc_overlay_init(&im->screen->overlay);
    sc_mapping_manager_init(&im->screen->mm);
    // The manager must be able to lift held fingers before it mutates entries.
    sc_mapping_manager_set_release_hook(&im->screen->mm, mapping_release_all, im);

    char status[160];
    snprintf(status, sizeof(status),
             "Template '%s': %d entries, %d rejected",
             im->screen->mm.active_name ? im->screen->mm.active_name : "-",
             im->screen->mm.count, im->screen->mm.last_load.fail);
    sc_overlay_set_status(&im->screen->overlay, status);
    for (int i = 0; i < im->screen->mm.last_load.nmsgs; i++) {
        LOGW("%s", im->screen->mm.last_load.msgs[i]);
    }
}

// ==================== SCRCPY 原有按键动作 ====================

static void
send_keycode(struct sc_input_manager *im, enum android_keycode keycode,
             enum sc_action action, const char *name) {
    assert(im->controller && im->kp && !im->camera);

    struct sc_control_msg msg;
    msg.type = SC_CONTROL_MSG_TYPE_INJECT_KEYCODE;
    msg.inject_keycode.action = action == SC_ACTION_DOWN
                              ? AKEY_EVENT_ACTION_DOWN
                              : AKEY_EVENT_ACTION_UP;
    msg.inject_keycode.keycode = keycode;
    msg.inject_keycode.metastate = 0;
    msg.inject_keycode.repeat = 0;

    if (!sc_controller_push_msg(im->controller, &msg)) {
        LOGW("Could not request 'inject %s'", name);
    }
}

static inline void
action_home(struct sc_input_manager *im, enum sc_action action) {
    send_keycode(im, AKEYCODE_HOME, action, "HOME");
}

static inline void
action_back(struct sc_input_manager *im, enum sc_action action) {
    send_keycode(im, AKEYCODE_BACK, action, "BACK");
}

static inline void
action_app_switch(struct sc_input_manager *im, enum sc_action action) {
    send_keycode(im, AKEYCODE_APP_SWITCH, action, "APP_SWITCH");
}

static inline void
action_power(struct sc_input_manager *im, enum sc_action action) {
    send_keycode(im, AKEYCODE_POWER, action, "POWER");
}

static inline void
action_volume_up(struct sc_input_manager *im, enum sc_action action) {
    send_keycode(im, AKEYCODE_VOLUME_UP, action, "VOLUME_UP");
}

static inline void
action_volume_down(struct sc_input_manager *im, enum sc_action action) {
    send_keycode(im, AKEYCODE_VOLUME_DOWN, action, "VOLUME_DOWN");
}

static inline void
action_menu(struct sc_input_manager *im, enum sc_action action) {
    send_keycode(im, AKEYCODE_MENU, action, "MENU");
}

static void
press_back_or_turn_screen_on(struct sc_input_manager *im,
                             enum sc_action action) {
    assert(im->controller && im->kp && !im->camera);

    struct sc_control_msg msg;
    msg.type = SC_CONTROL_MSG_TYPE_BACK_OR_SCREEN_ON;
    msg.back_or_screen_on.action = action == SC_ACTION_DOWN
                                 ? AKEY_EVENT_ACTION_DOWN
                                 : AKEY_EVENT_ACTION_UP;

    if (!sc_controller_push_msg(im->controller, &msg)) {
        LOGW("Could not request 'press back or turn screen on'");
    }
}

static void
expand_notification_panel(struct sc_input_manager *im) {
    assert(im->controller && !im->camera);
    struct sc_control_msg msg;
    msg.type = SC_CONTROL_MSG_TYPE_EXPAND_NOTIFICATION_PANEL;
    if (!sc_controller_push_msg(im->controller, &msg)) {
        LOGW("Could not request 'expand notification panel'");
    }
}

static void
expand_settings_panel(struct sc_input_manager *im) {
    assert(im->controller && !im->camera);
    struct sc_control_msg msg;
    msg.type = SC_CONTROL_MSG_TYPE_EXPAND_SETTINGS_PANEL;
    if (!sc_controller_push_msg(im->controller, &msg)) {
        LOGW("Could not request 'expand settings panel'");
    }
}

static void
collapse_panels(struct sc_input_manager *im) {
    assert(im->controller && !im->camera);
    struct sc_control_msg msg;
    msg.type = SC_CONTROL_MSG_TYPE_COLLAPSE_PANELS;
    if (!sc_controller_push_msg(im->controller, &msg)) {
        LOGW("Could not request 'collapse notification panel'");
    }
}

static bool
get_device_clipboard(struct sc_input_manager *im, enum sc_copy_key copy_key) {
    assert(im->controller && im->kp && !im->camera);
    struct sc_control_msg msg;
    msg.type = SC_CONTROL_MSG_TYPE_GET_CLIPBOARD;
    msg.get_clipboard.copy_key = copy_key;
    if (!sc_controller_push_msg(im->controller, &msg)) {
        LOGW("Could not request 'get device clipboard'");
        return false;
    }
    return true;
}

static bool
set_device_clipboard(struct sc_input_manager *im, bool paste,
                     uint64_t sequence) {
    assert(im->controller && im->kp && !im->camera);

    char *text = SDL_GetClipboardText();
    if (!text) {
        LOGW("Could not get clipboard text: %s", SDL_GetError());
        return false;
    }

    char *text_dup = strdup(text);
    SDL_free(text);
    if (!text_dup) {
        LOGW("Could not strdup input text");
        return false;
    }

    struct sc_control_msg msg;
    msg.type = SC_CONTROL_MSG_TYPE_SET_CLIPBOARD;
    msg.set_clipboard.sequence = sequence;
    msg.set_clipboard.text = text_dup;
    msg.set_clipboard.paste = paste;

    if (!sc_controller_push_msg(im->controller, &msg)) {
        free(text_dup);
        LOGW("Could not request 'set device clipboard'");
        return false;
    }
    return true;
}

static void
set_display_power(struct sc_input_manager *im, bool on) {
    assert(im->controller && !im->camera);
    struct sc_control_msg msg;
    msg.type = SC_CONTROL_MSG_TYPE_SET_DISPLAY_POWER;
    msg.set_display_power.on = on;
    if (!sc_controller_push_msg(im->controller, &msg)) {
        LOGW("Could not request 'set screen power mode'");
    }
}

static void
switch_fps_counter_state(struct sc_input_manager *im) {
    struct sc_fps_counter *fps_counter = &im->screen->fps_counter;
    if (sc_fps_counter_is_started(fps_counter)) {
        sc_fps_counter_stop(fps_counter);
    } else {
        sc_fps_counter_start(fps_counter);
    }
}

static void
clipboard_paste(struct sc_input_manager *im) {
    assert(im->controller && im->kp && !im->camera);

    char *text = SDL_GetClipboardText();
    if (!text) {
        LOGW("Could not get clipboard text: %s", SDL_GetError());
        return;
    }
    if (!*text) {
        SDL_free(text);
        return;
    }

    char *text_dup = strdup(text);
    SDL_free(text);
    if (!text_dup) {
        LOGW("Could not strdup input text");
        return;
    }

    struct sc_control_msg msg;
    msg.type = SC_CONTROL_MSG_TYPE_INJECT_TEXT;
    msg.inject_text.text = text_dup;
    if (!sc_controller_push_msg(im->controller, &msg)) {
        free(text_dup);
        LOGW("Could not request 'paste clipboard'");
    }
}

static void
rotate_device(struct sc_input_manager *im) {
    assert(im->controller && !im->camera);
    struct sc_control_msg msg;
    msg.type = SC_CONTROL_MSG_TYPE_ROTATE_DEVICE;
    if (!sc_controller_push_msg(im->controller, &msg)) {
        LOGW("Could not request device rotation");
    }
}

static void
open_hard_keyboard_settings(struct sc_input_manager *im) {
    assert(im->controller && !im->camera);
    struct sc_control_msg msg;
    msg.type = SC_CONTROL_MSG_TYPE_OPEN_HARD_KEYBOARD_SETTINGS;
    if (!sc_controller_push_msg(im->controller, &msg)) {
        LOGW("Could not request opening hard keyboard settings");
    }
}

static void
reset_video(struct sc_input_manager *im) {
    assert(im->controller);
    struct sc_control_msg msg;
    msg.type = SC_CONTROL_MSG_TYPE_RESET_VIDEO;
    if (!sc_controller_push_msg(im->controller, &msg)) {
        LOGW("Could not request reset video");
    }
}

static void
camera_set_torch(struct sc_input_manager *im, bool on) {
    assert(im->controller && im->camera);
    struct sc_control_msg msg;
    msg.type = SC_CONTROL_MSG_TYPE_CAMERA_SET_TORCH;
    msg.camera_set_torch.on = on;
    if (!sc_controller_push_msg(im->controller, &msg)) {
        LOGW("Could not request setting camera torch");
    }
}

static void
camera_zoom_in(struct sc_input_manager *im) {
    assert(im->controller && im->camera);
    struct sc_control_msg msg;
    msg.type = SC_CONTROL_MSG_TYPE_CAMERA_ZOOM_IN;
    if (!sc_controller_push_msg(im->controller, &msg)) {
        LOGW("Could not request camera zoom in");
    }
}

static void
camera_zoom_out(struct sc_input_manager *im) {
    assert(im->controller && im->camera);
    struct sc_control_msg msg;
    msg.type = SC_CONTROL_MSG_TYPE_CAMERA_ZOOM_OUT;
    if (!sc_controller_push_msg(im->controller, &msg)) {
        LOGW("Could not request camera zoom out");
    }
}

static void
apply_orientation_transform(struct sc_input_manager *im,
                            enum sc_orientation transform) {
    struct sc_screen *screen = im->screen;
    enum sc_orientation new_orientation =
        sc_orientation_apply(screen->orientation, transform);
    sc_screen_set_orientation(screen, new_orientation);
}

static void
sc_input_manager_process_text_input(struct sc_input_manager *im,
                                    const SDL_TextInputEvent *event) {
    if (im->camera || !im->kp || im->screen->paused || im->disconnected) {
        return;
    }
    if (!im->kp->ops->process_text) return;
    if (overlay_manager_open(im)) return; // panel is modal: no text forwarding

    uint16_t mod = sc_sdl_mod_normalize(SDL_GetModState());
    if (sc_shortcut_mods_is_shortcut_mod(im->sdl_shortcut_mods, mod)) return;

    struct sc_text_event evt = { .text = event->text };
    im->kp->ops->process_text(im->kp, &evt);
}

static bool
simulate_virtual_finger(struct sc_input_manager *im,
                        enum android_motionevent_action action,
                        struct sc_point point) {
    bool up = action == AMOTION_EVENT_ACTION_UP;
    struct sc_control_msg msg;
    msg.type = SC_CONTROL_MSG_TYPE_INJECT_TOUCH_EVENT;
    msg.inject_touch_event.action = action;
    msg.inject_touch_event.position.screen_size = im->screen->frame_size;
    msg.inject_touch_event.position.point = point;
    msg.inject_touch_event.pointer_id = SC_POINTER_ID_VIRTUAL_FINGER;
    msg.inject_touch_event.pressure = up ? 0.0f : 1.0f;
    msg.inject_touch_event.action_button = 0;
    msg.inject_touch_event.buttons = 0;
    if (!sc_controller_push_msg(im->controller, &msg)) {
        LOGW("Could not request 'inject virtual finger event'");
        return false;
    }
    return true;
}

static struct sc_point
inverse_point(struct sc_point point, struct sc_size size,
              bool invert_x, bool invert_y) {
    if (invert_x) point.x = size.width - point.x;
    if (invert_y) point.y = size.height - point.y;
    return point;
}

// ==================== 按键处理（含自定义映射拦截） ====================

static void
sc_input_manager_process_key(struct sc_input_manager *im,
                             const SDL_KeyboardEvent *event) {
    bool control = im->controller;
    bool paused = im->screen->paused;
    bool video = im->screen->video;
    bool disconnected = im->disconnected;

    SDL_Keycode sdl_keycode = event->key;
    uint16_t mod = sc_sdl_mod_normalize(event->mod);
    bool down = event->type == SDL_EVENT_KEY_DOWN;
    bool ctrl = mod & SDL_KMOD_CTRL;
    bool shift = mod & SDL_KMOD_SHIFT;
    bool repeat = event->repeat;

    struct sc_mapping_manager *mm = mm_of(im);
    struct sc_overlay *ov = ov_of(im);

    // ---- 管理面板（F5）打开时：键盘归面板独占 ----
    // F5 关闭；F3 关闭面板并切到设计模式（二者互斥）；其余按键交给面板，
    // 一律不转发到设备。
    if (overlay_manager_open(im)) {
        if (down && !repeat) {
            if (sdl_keycode == mm->sc_manager) {
                manager_close(im);
            } else if (sdl_keycode == mm->sc_design) {
                manager_close(im);
                toggle_design_mode(im);
            } else {
                manager_handle_key(im, sdl_keycode);
            }
        }
        return;
    }

    // ---- 全局快捷键（无论映射是否启用都响应） ----
    if (down && !repeat) {
        if (sdl_keycode == mm->sc_manager) {
            manager_open(im);
            return;
        }
        if (sdl_keycode == mm->sc_switch) {
            if (im->switch_open) close_template_switch(im);
            else open_template_switch(im);
            return;
        }
        // Esc closes the switch popup
        if (im->switch_open && sdl_keycode == SDLK_ESCAPE) {
            close_template_switch(im);
            return;
        }
        if (sdl_keycode == mm->sc_design) {
            toggle_design_mode(im);
            return;
        }
        if (sdl_keycode == mm->sc_toggle) {
            bool new_state = !mm->enabled;
            sc_mapping_manager_set_enabled(mm, new_state);
            sc_overlay_show_toggle(ov, new_state);
            return;
        }
    }

    // While the switch popup is open, suppress all other key input
    if (im->switch_open) {
        return;
    }

    // ---- 设计模式：按键绑定到待记录位置 ----
    if (im->design_mode && down && !repeat) {
        // Ignore modifier-only keys in design bind
        if (sdl_keycode != SDLK_LCTRL && sdl_keycode != SDLK_RCTRL &&
            sdl_keycode != SDLK_LSHIFT && sdl_keycode != SDLK_RSHIFT &&
            sdl_keycode != SDLK_LALT && sdl_keycode != SDLK_RALT &&
            sdl_keycode != SDLK_LGUI && sdl_keycode != SDLK_RGUI) {
            const char *name = sc_mapping_key_name(sdl_keycode);
            if (sc_mapping_manager_design_bind(mm, name)) {
                char buf[128];
                snprintf(buf, sizeof(buf), "Bound %s", name);
                sc_overlay_set_status(ov, buf);
            } else if (!sc_mapping_manager_design_has_point(mm)) {
                sc_overlay_set_status(ov,
                    "Click a position first, then press a key");
            }
            return;
        }
    }

    // ---- 自定义映射拦截（仅当启用且非设计模式） ----
    if (mm->enabled && !im->design_mode && control && !paused
            && !disconnected && !im->camera) {
        // 左 Alt 切换左键锁定
        if (sdl_keycode == SDLK_LALT && down && !repeat && !ctrl && !shift) {
            im->left_click_locked = !im->left_click_locked;
            im->left_click_locked_down = false;
            LOGI("Left click lock: %s", im->left_click_locked ? "ON" : "OFF");
            return;
        }

        bool is_mapping_key = sc_mapping_manager_find_single(mm, sdl_keycode) != NULL;
        bool is_combo_key = sc_mapping_manager_key_in_combo(mm, sdl_keycode);

        if (is_mapping_key || is_combo_key) {
            if (down && repeat) return;  // 忽略键盘自动重复

            if (down) im->g_pressed[sdl_keycode & 0xFF] = true;
            else im->g_pressed[sdl_keycode & 0xFF] = false;

            update_combos(im);

            struct sc_mapping_entry *m =
                sc_mapping_manager_find_single(mm, sdl_keycode);
            if (m) {
                // 如果该键被已激活的 combo 覆盖，跳过单键逻辑
                bool covered = false;
                for (int i = 0; i < SC_MAPPING_MAX_ENTRIES; ++i) {
                    struct sc_mapping_entry *c = &mm->entries[i];
                    if (!c->key_name || !c->active) continue;
                    for (int j = 0; j < c->nkeys; ++j) {
                        if (c->keys[j] == sdl_keycode) { covered = true; break; }
                    }
                    if (covered) break;
                }
                if (!covered) {
                    if (down && m->down) return;
                    if (!down && !m->down) return;
                    if (m->type == SC_MAPPING_TYPE_SLIDE) {
                        // hold-to-slide: press starts the slide, release stops
                        if (down) {
                            m->down = true;
                            slide_start(im, m);
                        } else {
                            m->down = false;
                            slide_stop_for_entry(im, m);
                        }
                    } else {
                        enum android_motionevent_action action = down
                            ? AMOTION_EVENT_ACTION_DOWN
                            : AMOTION_EVENT_ACTION_UP;
                        inject_entry(im, m, action);
                        m->down = down;
                    }
                } else if (!down && m->down) {
                    m->down = false;
                    if (m->type == SC_MAPPING_TYPE_SLIDE) {
                        slide_stop_for_entry(im, m);
                    } else {
                        inject_entry(im, m, AMOTION_EVENT_ACTION_UP);
                    }
                }
            }
            return;
        }
    }

    // ---- 以下为 SCRCPY 原有逻辑 ----

    uint16_t mods = im->sdl_shortcut_mods;
    bool is_shortcut = sc_shortcut_mods_is_shortcut_mod(mods, mod)
                    || sc_shortcut_mods_is_shortcut_key(mods, sdl_keycode);

    if (down && !repeat && !disconnected) {
        if (sdl_keycode == im->last_keycode && mod == im->last_mod) {
            ++im->key_repeat;
        } else {
            im->key_repeat = 0;
            im->last_keycode = sdl_keycode;
            im->last_mod = mod;
        }
    }

    switch (sdl_keycode) {
        case SDLK_F11:
            if (video && !repeat && down) {
                bool alt = mod & SDL_KMOD_ALT;
                bool super = mod & SDL_KMOD_GUI;
                if (!ctrl && !shift && !alt && !super) {
                    sc_screen_toggle_fullscreen(im->screen);
                }
            }
            return;
    }

    if (is_shortcut) {
        enum sc_action action = down ? SC_ACTION_DOWN : SC_ACTION_UP;
        switch (sdl_keycode) {
            case SDLK_Z:
                if (video && down && !repeat) sc_screen_set_paused(im->screen, !shift);
                return;
            case SDLK_DOWN:
                if (shift) {
                    if (video && !repeat && down) apply_orientation_transform(im, SC_ORIENTATION_FLIP_180);
                    return;
                }
                break;
            case SDLK_UP:
                if (shift) {
                    if (video && !repeat && down) apply_orientation_transform(im, SC_ORIENTATION_FLIP_180);
                    return;
                }
                break;
            case SDLK_LEFT:
                if (video && !repeat && down) {
                    if (shift) apply_orientation_transform(im, SC_ORIENTATION_FLIP_0);
                    else apply_orientation_transform(im, SC_ORIENTATION_270);
                }
                return;
            case SDLK_RIGHT:
                if (video && !repeat && down) {
                    if (shift) apply_orientation_transform(im, SC_ORIENTATION_FLIP_0);
                    else apply_orientation_transform(im, SC_ORIENTATION_90);
                }
                return;
            case SDLK_F:
                if (video && !shift && !repeat && down) sc_screen_toggle_fullscreen(im->screen);
                return;
            case SDLK_W:
                if (video && !shift && !repeat && down) sc_screen_resize_to_fit(im->screen);
                return;
            case SDLK_G:
                if (video && !shift && !repeat && down) sc_screen_resize_to_pixel_perfect(im->screen);
                return;
            case SDLK_I:
                if (video && !shift && !repeat && down) switch_fps_counter_state(im);
                return;
            case SDLK_Q:
                sc_push_event(SDL_EVENT_QUIT);
                return;
        }

        if (disconnected) return;

        if (control) {
            switch (sdl_keycode) {
                case SDLK_R:
                    if (shift) {
                        if (!repeat && down && !paused) reset_video(im);
                        return;
                    }
                    break;
            }
        }

        if (control && !im->camera) {
            switch (sdl_keycode) {
                case SDLK_H:
                    if (im->kp && !shift && !repeat && !paused) action_home(im, action);
                    return;
                case SDLK_B:
                case SDLK_BACKSPACE:
                    if (im->kp && !shift && !repeat && !paused) action_back(im, action);
                    return;
                case SDLK_S:
                    if (im->kp && !shift && !repeat && !paused) action_app_switch(im, action);
                    return;
                case SDLK_M:
                    if (im->kp && !shift && !repeat && !paused) action_menu(im, action);
                    return;
                case SDLK_P:
                    if (im->kp && !shift && !repeat && !paused) action_power(im, action);
                    return;
                case SDLK_O:
                    if (control && !repeat && down && !paused) {
                        bool on = shift;
                        set_display_power(im, on);
                    }
                    return;
                case SDLK_DOWN:
                    if (im->kp && !shift && !paused) action_volume_down(im, action);
                    return;
                case SDLK_UP:
                    if (im->kp && !shift && !paused) action_volume_up(im, action);
                    return;
                case SDLK_C:
                    if (im->kp && !shift && !repeat && down && !paused) get_device_clipboard(im, SC_COPY_KEY_COPY);
                    return;
                case SDLK_X:
                    if (im->kp && !shift && !repeat && down && !paused) get_device_clipboard(im, SC_COPY_KEY_CUT);
                    return;
                case SDLK_V:
                    if (im->kp && !repeat && down && !paused) {
                        if (shift || im->legacy_paste) clipboard_paste(im);
                        else set_device_clipboard(im, true, SC_SEQUENCE_INVALID);
                    }
                    return;
                case SDLK_N:
                    if (!repeat && down && !paused) {
                        if (shift) collapse_panels(im);
                        else if (im->key_repeat == 0) expand_notification_panel(im);
                        else expand_settings_panel(im);
                    }
                    return;
                case SDLK_R:
                    if (!repeat && !shift && down && !paused) rotate_device(im);
                    return;
                case SDLK_K:
                    if (!shift && !repeat && down && !paused && im->kp && im->kp->hid)
                        open_hard_keyboard_settings(im);
                    return;
            }
        }

        if (control && im->camera) {
            switch (sdl_keycode) {
                case SDLK_T:
                    if (!repeat && down) camera_set_torch(im, !shift);
                    return;
                case SDLK_DOWN:
                    if (!shift && down && !paused) camera_zoom_out(im);
                    return;
                case SDLK_UP:
                    if (!shift && down && !paused) camera_zoom_in(im);
                    return;
            }
        }
        return;
    }

    if (!im->kp || paused) return;
    assert(!im->camera);

    uint64_t ack_to_wait = SC_SEQUENCE_INVALID;
    bool is_ctrl_v = ctrl && !shift && sdl_keycode == SDLK_V && down && !repeat;
    if (im->clipboard_autosync && is_ctrl_v) {
        if (im->legacy_paste) {
            clipboard_paste(im);
            return;
        }
        uint64_t sequence = im->kp->async_paste ? im->next_sequence : SC_SEQUENCE_INVALID;
        bool ok = set_device_clipboard(im, false, sequence);
        if (!ok) {
            LOGW("Clipboard could not be synchronized, Ctrl+v not injected");
            return;
        }
        if (im->kp->async_paste) {
            ack_to_wait = sequence;
            ++im->next_sequence;
        }
    }

    enum sc_keycode keycode = sc_keycode_from_sdl(sdl_keycode);
    enum sc_scancode scancode = sc_scancode_from_sdl(event->scancode);
    if (scancode == SC_SCANCODE_UNKNOWN) return;

    struct sc_key_event evt = {
        .action = sc_action_from_sdl_keyboard_type(event->type),
        .keycode = keycode,
        .scancode = scancode,
        .repeat = event->repeat,
        .mods_state = sc_mods_state_from_sdl(mod),
    };
    assert(im->kp->ops->process_key);
    im->kp->ops->process_key(im->kp, &evt, ack_to_wait);
}

static struct sc_position
sc_input_manager_get_position(struct sc_input_manager *im, int32_t x, int32_t y) {
    if (im->mp->relative_mode) {
        return (struct sc_position) { .screen_size = {0, 0}, .point = {0, 0} };
    }
    return (struct sc_position) {
        .screen_size = im->screen->frame_size,
        .point = sc_screen_convert_window_to_frame_coords(im->screen, x, y),
    };
}

static void
sc_input_manager_process_mouse_motion(struct sc_input_manager *im,
                                      const SDL_MouseMotionEvent *event) {
    if (im->camera || !im->mp || im->screen->paused || im->disconnected) return;
    if (event->which == SDL_TOUCH_MOUSEID) return;

    // 管理面板打开时鼠标移动不转发到设备（面板本身由键盘驱动）；面板关闭后
    // 转发行为与打开前完全一致。
    if (overlay_manager_open(im)) return;

    // If switch popup is open, update hover (and request a debounced switch)
    if (im->switch_open) {
        int hovered = sc_overlay_update_switch_mouse(ov_of(im),
                                                     (float)event->x,
                                                     (float)event->y);
        if (hovered >= 0) {
            char **names = ov_of(im)->names;
            if (names && names[hovered]) {
                // No file I/O here: the switch is applied later by the tick,
                // after the hover has been stable for a short delay.
                sc_mapping_manager_request_switch(mm_of(im), names[hovered]);
            }
        }
        return;
    }

    struct sc_mouse_motion_event evt = {
        .position = sc_input_manager_get_position(im, event->x, event->y),
        .pointer_id = im->vfinger_down ? SC_POINTER_ID_GENERIC_FINGER : SC_POINTER_ID_MOUSE,
        .xrel = event->xrel,
        .yrel = event->yrel,
        .buttons_state = im->mouse_buttons_state,
    };
    assert(im->mp->ops->process_mouse_motion);
    im->mp->ops->process_mouse_motion(im->mp, &evt);

    if (im->vfinger_down) {
        struct sc_point mouse = sc_screen_convert_window_to_frame_coords(im->screen, event->x, event->y);
        struct sc_point vfinger = inverse_point(mouse, im->screen->frame_size, im->vfinger_invert_x, im->vfinger_invert_y);
        simulate_virtual_finger(im, AMOTION_EVENT_ACTION_MOVE, vfinger);
    }
}

static void
sc_input_manager_process_touch(struct sc_input_manager *im,
                               const SDL_TouchFingerEvent *event) {
    if (im->camera || !im->mp || im->screen->paused || im->disconnected) return;
    if (!im->mp->ops->process_touch) return;
    if (overlay_manager_open(im)) return; // panel is modal: no touch forwarding

    struct sc_size window_size = sc_sdl_get_window_size(im->screen->window);
    int32_t x = event->x * (int32_t)window_size.width;
    int32_t y = event->y * (int32_t)window_size.height;

    struct sc_touch_event evt = {
        .position = {
            .screen_size = im->screen->frame_size,
            .point = sc_screen_convert_window_to_frame_coords(im->screen, x, y),
        },
        .action = sc_touch_action_from_sdl(event->type),
        .pointer_id = event->fingerID,
        .pressure = event->pressure,
    };
    im->mp->ops->process_touch(im->mp, &evt);
}

static enum sc_mouse_binding
sc_input_manager_get_binding(const struct sc_mouse_binding_set *bindings,
                             uint8_t sdl_button) {
    switch (sdl_button) {
        case SDL_BUTTON_LEFT: return SC_MOUSE_BINDING_CLICK;
        case SDL_BUTTON_RIGHT: return bindings->right_click;
        case SDL_BUTTON_MIDDLE: return bindings->middle_click;
        case SDL_BUTTON_X1: return bindings->click4;
        case SDL_BUTTON_X2: return bindings->click5;
        default: return SC_MOUSE_BINDING_DISABLED;
    }
}

static void
sc_input_manager_process_mouse_button(struct sc_input_manager *im,
                                      const SDL_MouseButtonEvent *event) {
    if (im->camera || im->disconnected) return;
    if (event->which == SDL_TOUCH_MOUSEID) return;

    bool control = im->controller;
    bool paused = im->screen->paused;
    bool down = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN;

    // 管理面板打开时鼠标归面板所有：左键用于列表选择 / 取点，其它键吞掉，
    // 全部不转发到设备。关闭面板时 manager_close() 会复位按键与锁定状态。
    if (overlay_manager_open(im)) {
        if (down && event->button == SDL_BUTTON_LEFT) {
            manager_handle_click(im, event->x, event->y);
        }
        return;
    }

    struct sc_mapping_manager *mm = mm_of(im);

    // 设计模式：右键切换记录类型（单点 / 滑动两点）
    if (im->design_mode && event->button == SDL_BUTTON_RIGHT && down) {
        mm->design_is_slide = !mm->design_is_slide;
        sc_mapping_manager_design_reset(mm);
        const char *hint = mm->design_is_slide
            ? "Design SLIDE: click twice (start / end), then a key. "
              "Right click = touch mode. F3 = exit."
            : "Design TOUCH: click a position, then a key. "
              "Right click = slide mode. F3 = exit.";
        sc_overlay_set_design(ov_of(im), true, hint);
        sc_overlay_set_status(ov_of(im), hint);
        return;
    }

    // 设计模式：左键点击记录位置
    if (im->design_mode && event->button == SDL_BUTTON_LEFT && down) {
        struct sc_point p = sc_screen_convert_window_to_frame_coords(
            im->screen, event->x, event->y);
        double nx = (double)p.x / im->screen->frame_size.width;
        double ny = (double)p.y / im->screen->frame_size.height;
        if (nx < 0) nx = 0;
        if (nx > 1) nx = 1;
        if (ny < 0) ny = 0;
        if (ny > 1) ny = 1;
        sc_mapping_manager_design_click(mm, nx, ny);
        char buf[160];
        if (mm->design_is_slide && mm->design_stage == 1) {
            snprintf(buf, sizeof(buf),
                     "Slide start (%.3f, %.3f). Click the end point.",
                     nx, ny);
        } else if (mm->design_is_slide && mm->design_stage == 2) {
            snprintf(buf, sizeof(buf),
                     "Slide recorded (d=%.3f, %.3f, %dms). Press a key.",
                     mm->design_dx, mm->design_dy, mm->design_duration_ms);
        } else {
            snprintf(buf, sizeof(buf),
                     "Position recorded (%.3f, %.3f). Press a key to bind.",
                     nx, ny);
        }
        sc_overlay_set_status(ov_of(im), buf);
        sc_overlay_set_design(ov_of(im), true, buf);
        return;
    }

    // 切换弹窗打开时，点击关闭
    if (im->switch_open) {
        close_template_switch(im);
        return;
    }

    // ==================== 自定义鼠标拦截 ====================
    if (mm->enabled && !im->design_mode && control && !paused) {
        // 左键锁定
        if (event->button == SDL_BUTTON_LEFT && im->left_click_locked) {
            if (down == im->left_click_locked_down) return;
            im->left_click_locked_down = down;
            enum android_motionevent_action action = down
                ? AMOTION_EVENT_ACTION_DOWN : AMOTION_EVENT_ACTION_UP;
            // Index lookup, never a stored pointer (entries are a fixed table
            // and are replaced on reload)
            int idx = mm->mouse_left_locked_idx;
            if (idx >= 0 && mm->entries[idx].key_name) {
                inject_entry(im, &mm->entries[idx], action);
            }
            return;
        }

        // 右键固定映射
        if (event->button == SDL_BUTTON_RIGHT) {
            if (down == im->right_click_down) return;
            im->right_click_down = down;
            enum android_motionevent_action action = down
                ? AMOTION_EVENT_ACTION_DOWN : AMOTION_EVENT_ACTION_UP;
            int idx = mm->mouse_right_idx;
            if (idx >= 0 && mm->entries[idx].key_name) {
                inject_entry(im, &mm->entries[idx], action);
            }
            return;
        }
    }

    enum sc_mouse_button button = sc_mouse_button_from_sdl(event->button);
    if (button == SC_MOUSE_BUTTON_UNKNOWN) return;

    if (!down) im->mouse_buttons_state &= ~button;

    SDL_Keymod keymod = SDL_GetModState();
    bool ctrl_pressed = keymod & SDL_KMOD_CTRL;
    bool shift_pressed = keymod & SDL_KMOD_SHIFT;

    if (control && !paused) {
        enum sc_action action = down ? SC_ACTION_DOWN : SC_ACTION_UP;
        struct sc_mouse_binding_set *bindings = !shift_pressed
                                              ? &im->mouse_bindings.pri
                                              : &im->mouse_bindings.sec;
        enum sc_mouse_binding binding = sc_input_manager_get_binding(bindings, event->button);
        assert(binding != SC_MOUSE_BINDING_AUTO);
        switch (binding) {
            case SC_MOUSE_BINDING_DISABLED: return;
            case SC_MOUSE_BINDING_BACK:
                if (im->kp) press_back_or_turn_screen_on(im, action);
                return;
            case SC_MOUSE_BINDING_HOME:
                if (im->kp) action_home(im, action);
                return;
            case SC_MOUSE_BINDING_APP_SWITCH:
                if (im->kp) action_app_switch(im, action);
                return;
            case SC_MOUSE_BINDING_EXPAND_NOTIFICATION_PANEL:
                if (down) {
                    if (event->clicks < 2) expand_notification_panel(im);
                    else expand_settings_panel(im);
                }
                return;
            default: assert(binding == SC_MOUSE_BINDING_CLICK); break;
        }
    }

    bool video = im->screen->video;
    bool mouse_relative_mode = im->mp && im->mp->relative_mode;
    if (video && !mouse_relative_mode && event->button == SDL_BUTTON_LEFT && event->clicks == 2) {
        int32_t x = event->x;
        int32_t y = event->y;
        SDL_FRect *r = &im->screen->rect;
        bool outside = x < r->x || x >= r->x + r->w || y < r->y || y >= r->y + r->h;
        if (outside) {
            if (down) sc_screen_resize_to_fit(im->screen);
            return;
        }
    }

    if (!im->mp || paused) return;

    if (down) im->mouse_buttons_state |= button;

    bool change_vfinger = event->button == SDL_BUTTON_LEFT &&
            ((down && !im->vfinger_down && (ctrl_pressed || shift_pressed)) ||
             (!down && im->vfinger_down));
    bool use_finger = im->vfinger_down || change_vfinger;

    struct sc_mouse_click_event evt = {
        .position = sc_input_manager_get_position(im, event->x, event->y),
        .action = sc_action_from_sdl_mousebutton_type(event->type),
        .button = button,
        .pointer_id = use_finger ? SC_POINTER_ID_GENERIC_FINGER : SC_POINTER_ID_MOUSE,
        .buttons_state = im->mouse_buttons_state,
    };
    assert(im->mp->ops->process_mouse_click);
    im->mp->ops->process_mouse_click(im->mp, &evt);

    if (im->mp->relative_mode) return;

    if (change_vfinger) {
        struct sc_point mouse = sc_screen_convert_window_to_frame_coords(im->screen, event->x, event->y);
        if (down) {
            im->vfinger_invert_x = ctrl_pressed ^ shift_pressed;
            im->vfinger_invert_y = ctrl_pressed;
        }
        struct sc_point vfinger = inverse_point(mouse, im->screen->frame_size, im->vfinger_invert_x, im->vfinger_invert_y);
        enum android_motionevent_action action = down ? AMOTION_EVENT_ACTION_DOWN : AMOTION_EVENT_ACTION_UP;
        if (!simulate_virtual_finger(im, action, vfinger)) return;
        im->vfinger_down = down;
    }
}

static void
sc_input_manager_process_mouse_wheel(struct sc_input_manager *im,
                                     const SDL_MouseWheelEvent *event) {
    if (im->camera || !im->kp || im->screen->paused || im->disconnected) return;
    if (!im->mp->ops->process_mouse_scroll) return;
    if (overlay_manager_open(im)) return; // panel is modal: no scrolling

    float mouse_x, mouse_y;
    uint32_t buttons = SDL_GetMouseState(&mouse_x, &mouse_y);
    (void)buttons;

    struct sc_mouse_scroll_event evt = {
        .position = sc_input_manager_get_position(im, (int32_t)mouse_x, (int32_t)mouse_y),
        .hscroll = event->x,
        .vscroll = event->y,
        .buttons_state = im->mouse_buttons_state,
    };
    im->mp->ops->process_mouse_scroll(im->mp, &evt);
}

static void
sc_input_manager_process_gamepad_device(struct sc_input_manager *im,
                                       const SDL_GamepadDeviceEvent *event) {
    if (im->camera || !im->gp || im->disconnected) return;

    if (event->type == SDL_EVENT_GAMEPAD_ADDED) {
        SDL_Gamepad *sdl_gamepad = SDL_OpenGamepad(event->which);
        if (!sdl_gamepad) {
            LOGW("Could not open gamepad: %s", SDL_GetError());
            return;
        }
        SDL_Joystick *joystick = SDL_GetGamepadJoystick(sdl_gamepad);
        if (!joystick) {
            LOGW("Could not get gamepad joystick: %s", SDL_GetError());
            SDL_CloseGamepad(sdl_gamepad);
            return;
        }
        struct sc_gamepad_device_event evt = { .gamepad_id = SDL_GetJoystickID(joystick) };
        im->gp->ops->process_gamepad_added(im->gp, &evt);
    } else if (event->type == SDL_EVENT_GAMEPAD_REMOVED) {
        SDL_JoystickID id = event->which;
        SDL_Gamepad *sdl_gamepad = SDL_GetGamepadFromID(id);
        if (sdl_gamepad) SDL_CloseGamepad(sdl_gamepad);
        else LOGW("Unknown gamepad device removed");
        struct sc_gamepad_device_event evt = { .gamepad_id = id };
        im->gp->ops->process_gamepad_removed(im->gp, &evt);
    }
}

static void
sc_input_manager_process_gamepad_axis(struct sc_input_manager *im,
                                      const SDL_GamepadAxisEvent *event) {
    if (im->camera || !im->gp || im->screen->paused || im->disconnected) return;
    enum sc_gamepad_axis axis = sc_gamepad_axis_from_sdl(event->axis);
    if (axis == SC_GAMEPAD_AXIS_UNKNOWN) return;
    struct sc_gamepad_axis_event evt = {
        .gamepad_id = event->which,
        .axis = axis,
        .value = event->value,
    };
    im->gp->ops->process_gamepad_axis(im->gp, &evt);
}

static void
sc_input_manager_process_gamepad_button(struct sc_input_manager *im,
                                       const SDL_GamepadButtonEvent *event) {
    if (im->camera || !im->gp || im->screen->paused || im->disconnected) return;
    enum sc_gamepad_button button = sc_gamepad_button_from_sdl(event->button);
    if (button == SC_GAMEPAD_BUTTON_UNKNOWN) return;
    struct sc_gamepad_button_event evt = {
        .gamepad_id = event->which,
        .action = sc_action_from_sdl_gamepad_button_type(event->type),
        .button = button,
    };
    im->gp->ops->process_gamepad_button(im->gp, &evt);
}

static bool
is_apk(const char *file) {
    const char *ext = strrchr(file, '.');
    return ext && !strcmp(ext, ".apk");
}

static void
sc_input_manager_process_file(struct sc_input_manager *im,
                              const SDL_DropEvent *event) {
    if (im->camera || !im->controller || im->disconnected) return;
    assert(event->type == SDL_EVENT_DROP_FILE);
    char *file = strdup(event->data);
    if (!file) {
        LOG_OOM();
        return;
    }
    enum sc_file_pusher_action action;
    if (is_apk(file)) action = SC_FILE_PUSHER_ACTION_INSTALL_APK;
    else action = SC_FILE_PUSHER_ACTION_PUSH_FILE;
    bool ok = sc_file_pusher_request(im->fp, action, file);
    if (!ok) free(file);
}

static void
sc_input_manager_on_device_disconnected(struct sc_input_manager *im) {
    im->disconnected = true;
    struct sc_fps_counter *fps_counter = &im->screen->fps_counter;
    if (sc_fps_counter_is_started(fps_counter)) sc_fps_counter_stop(fps_counter);
}

void
sc_input_manager_handle_event(struct sc_input_manager *im,
                              const SDL_Event *event) {
    switch (event->type) {
        case SDL_EVENT_TEXT_INPUT:
            sc_input_manager_process_text_input(im, &event->text);
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            sc_input_manager_process_key(im, &event->key);
            break;
        case SDL_EVENT_MOUSE_MOTION:
            sc_input_manager_process_mouse_motion(im, &event->motion);
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            sc_input_manager_process_mouse_wheel(im, &event->wheel);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            sc_input_manager_process_mouse_button(im, &event->button);
            break;
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_UP:
            sc_input_manager_process_touch(im, &event->tfinger);
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED:
            sc_input_manager_process_gamepad_device(im, &event->gdevice);
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            sc_input_manager_process_gamepad_axis(im, &event->gaxis);
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            sc_input_manager_process_gamepad_button(im, &event->gbutton);
            break;
        case SDL_EVENT_DROP_FILE:
            sc_input_manager_process_file(im, &event->drop);
            break;
        case SC_EVENT_DEVICE_DISCONNECTED:
            sc_input_manager_on_device_disconnected(im);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            // Losing focus must stop every slide (the key up will never arrive)
            slide_stop_all(im);
            break;
    }
}
