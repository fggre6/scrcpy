#include "overlay_ui.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "mapping_manager.h"
#include "util/log.h"
#include "util/tick.h"

#define POPUP_WIDTH 320
#define ROW_HEIGHT 40
#define POPUP_PADDING 12

#define MGR_ROW_H 18
#define MGR_LIST_W 170
#define MGR_HEADER_H 30
#define MGR_FOOTER_H 44
#define MGR_TEXT(x) ((float) (x))

void
sc_overlay_init(struct sc_overlay *ov) {
    memset(ov, 0, sizeof(*ov));
    ov->hovered = -1;
    ov->mgr_edit_field = -1;
    ov->mode = SC_OVERLAY_NONE;
}

static void
free_names(char **names, int count) {
    if (!names) return;
    for (int i = 0; i < count; i++) {
        free(names[i]);
    }
    free(names);
}

void
sc_overlay_destroy(struct sc_overlay *ov) {
    free_names(ov->names, ov->count);
    ov->names = NULL;
    ov->count = 0;
    free_names(ov->mgr_names, ov->mgr_count);
    ov->mgr_names = NULL;
    ov->mgr_count = 0;
}

void
sc_overlay_show_switch(struct sc_overlay *ov, char **names, int count) {
    sc_overlay_hide_switch(ov);
    ov->names = names;
    ov->count = count;
    ov->hovered = -1;
    ov->mode = SC_OVERLAY_TEMPLATE_PICKER;
}

void
sc_overlay_hide_switch(struct sc_overlay *ov) {
    free_names(ov->names, ov->count);
    ov->names = NULL;
    ov->count = 0;
    ov->hovered = -1;
    if (ov->mode == SC_OVERLAY_TEMPLATE_PICKER) {
        ov->mode = SC_OVERLAY_NONE;
    }
}

int
sc_overlay_update_switch_mouse(struct sc_overlay *ov, float mx, float my) {
    if (ov->mode != SC_OVERLAY_TEMPLATE_PICKER) return -1;
    const SDL_FRect *r = &ov->popup_rect;
    if (mx < r->x || mx >= r->x + r->w || my < r->y || my >= r->y + r->h) {
        ov->hovered = -1;
        return -1;
    }
    float rel_y = my - r->y - POPUP_PADDING;
    int idx = (int) (rel_y / ROW_HEIGHT);
    if (idx < 0 || idx >= ov->count) {
        ov->hovered = -1;
        return -1;
    }
    ov->hovered = idx;
    return idx;
}

void
sc_overlay_set_design(struct sc_overlay *ov, bool active, const char *hint) {
    if (active) {
        if (ov->mode == SC_OVERLAY_TEMPLATE_PICKER) {
            sc_overlay_hide_switch(ov);
        }
        ov->mode = SC_OVERLAY_DESIGN;
    } else if (ov->mode == SC_OVERLAY_DESIGN) {
        ov->mode = SC_OVERLAY_NONE;
    }
    if (hint) {
        strncpy(ov->design_hint, hint, sizeof(ov->design_hint) - 1);
        ov->design_hint[sizeof(ov->design_hint) - 1] = '\0';
    } else {
        ov->design_hint[0] = '\0';
    }
}

void
sc_overlay_set_manager(struct sc_overlay *ov, bool open,
                       struct sc_mapping_manager *mm, char **names,
                       int count) {
    if (open) {
        // Manager and design mode are mutually exclusive
        if (ov->mode == SC_OVERLAY_DESIGN || ov->mode == SC_OVERLAY_TEMPLATE_PICKER) {
            sc_overlay_hide_switch(ov);
            ov->design_hint[0] = '\0';
        }
        free_names(ov->mgr_names, ov->mgr_count);
        ov->mgr_names = names;
        ov->mgr_count = count;
        ov->mm = mm;
        ov->mode = SC_OVERLAY_MANAGER;
        ov->mgr_focus = 1;
        ov->mgr_tmpl_sel = 0;
        ov->mgr_entry_sel = 0;
        ov->mgr_edit_field = -1;
        ov->mgr_edit_len = 0;
        ov->mgr_capture_key = false;
        ov->mgr_pick = 0;
    } else {
        free_names(ov->mgr_names, ov->mgr_count);
        ov->mgr_names = NULL;
        ov->mgr_count = 0;
        ov->mgr_edit_field = -1;
        ov->mgr_capture_key = false;
        ov->mgr_pick = 0;
        if (ov->mode == SC_OVERLAY_MANAGER) {
            ov->mode = SC_OVERLAY_NONE;
        }
    }
}

void
sc_overlay_manager_set_hint(struct sc_overlay *ov, const char *hint) {
    if (hint) {
        strncpy(ov->mgr_hint, hint, sizeof(ov->mgr_hint) - 1);
        ov->mgr_hint[sizeof(ov->mgr_hint) - 1] = '\0';
    } else {
        ov->mgr_hint[0] = '\0';
    }
}

void
sc_overlay_show_toggle(struct sc_overlay *ov, bool enabled) {
    ov->show_toggle = true;
    ov->mapping_enabled = enabled;
    ov->toggle_until = sc_tick_now() + SC_TICK_FROM_MS(1500);
}

void
sc_overlay_set_status(struct sc_overlay *ov, const char *msg) {
    if (msg) {
        strncpy(ov->status, msg, sizeof(ov->status) - 1);
        ov->status[sizeof(ov->status) - 1] = '\0';
        ov->status_until = sc_tick_now() + SC_TICK_FROM_MS(3000);
    } else {
        ov->status[0] = '\0';
    }
}

// ============================================================
// Drawing helpers
// ============================================================

static void
draw_panel(SDL_Renderer *r, float x, float y, float w, float h,
           Uint8 br, Uint8 bg, Uint8 bb, Uint8 ba) {
    SDL_SetRenderDrawColor(r, br, bg, bb, ba);
    SDL_FRect rect = { x, y, w, h };
    SDL_RenderFillRect(r, &rect);
}

static void
draw_border(SDL_Renderer *r, float x, float y, float w, float h,
            Uint8 br, Uint8 bg, Uint8 bb, Uint8 ba) {
    SDL_SetRenderDrawColor(r, br, bg, bb, ba);
    SDL_FRect rect = { x, y, w, h };
    SDL_RenderRect(r, &rect);
}

static void
draw_text(SDL_Renderer *r, float x, float y, const char *text,
          Uint8 cr, Uint8 cg, Uint8 cb) {
    SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
    SDL_RenderDebugText(r, x, y, text);
}

static int
mgr_panel_width(int win_w) {
    int w = (int) (win_w * 0.56f);
    if (w > 760) w = 760;
    if (w < 420 && win_w > 420) w = 420;
    if (w > win_w) w = win_w;
    return w;
}

// Collect the occupied entry slots (index list) for the entries column.
static int
collect_entries(struct sc_mapping_manager *mm, int *idx, int max) {
    int n = 0;
    for (int i = 0; i < SC_MAPPING_MAX_ENTRIES && n < max; i++) {
        if (mm->entries[i].key_name) idx[n++] = i;
    }
    return n;
}

static void
render_manager(struct sc_overlay *ov, SDL_Renderer *renderer,
               int win_w, int win_h) {
    struct sc_mapping_manager *mm = ov->mm;
    if (!mm) return;

    int pw = mgr_panel_width(win_w);
    draw_panel(renderer, 0, 0, (float) pw, (float) win_h, 12, 12, 16, 225);
    draw_border(renderer, (float) (pw - 1), 0, 1, (float) win_h,
                90, 140, 220, 255);

    char line[220];
    snprintf(line, sizeof(line), "MANAGER  [%s]  mapping:%s   (F5 close)",
             mm->active_name ? mm->active_name : "-",
             mm->enabled ? "ON" : "OFF");
    draw_text(renderer, 10, 8, line, 120, 200, 255);
    snprintf(line, sizeof(line), "loaded: %d ok, %d rejected",
             mm->last_load.ok, mm->last_load.fail);
    draw_text(renderer, 10, 20, line, 160, 160, 160);

    float top = (float) (MGR_HEADER_H + 8);
    float list_h = (float) win_h - top - MGR_FOOTER_H;

    // ---- templates column ----
    draw_text(renderer, 10, top, "Templates", 150, 220, 150);
    int trows = (int) (list_h / MGR_ROW_H) - 1;
    if (trows < 1) trows = 1;
    int toff = 0;
    if (ov->mgr_tmpl_sel >= trows) toff = ov->mgr_tmpl_sel - trows + 1;
    for (int i = 0; i < trows && toff + i < ov->mgr_count; i++) {
        int k = toff + i;
        float y = top + MGR_ROW_H * (float) (i + 1);
        bool sel = (k == ov->mgr_tmpl_sel);
        if (sel) {
            draw_panel(renderer, 8, y - 2, (float) MGR_LIST_W - 16, MGR_ROW_H,
                       ov->mgr_focus == 0 ? 60 : 40,
                       ov->mgr_focus == 0 ? 120 : 90, 220, 200);
        }
        const char *n = ov->mgr_names && ov->mgr_names[k] ? ov->mgr_names[k] : "?";
        bool active = mm->active_name && strcmp(mm->active_name, n) == 0;
        snprintf(line, sizeof(line), "%s %s", active ? "*" : " ", n);
        draw_text(renderer, 12, y, line, 220, 220, 220);
    }

    // ---- entries column ----
    float ex = (float) MGR_LIST_W + 8;
    draw_text(renderer, ex, top, "Entries", 150, 220, 150);
    int idx[SC_MAPPING_MAX_ENTRIES];
    int n = collect_entries(mm, idx, SC_MAPPING_MAX_ENTRIES);
    int erows = trows;
    if (erows < 1) erows = 1;
    if (ov->mgr_entry_sel > n - 1) ov->mgr_entry_sel = n > 0 ? n - 1 : 0;
    int eoff = 0;
    if (ov->mgr_entry_sel >= erows) eoff = ov->mgr_entry_sel - erows + 1;
    if (n == 0) {
        draw_text(renderer, ex, top + MGR_ROW_H, "(empty)", 140, 140, 140);
    }
    for (int i = 0; i < erows && eoff + i < n; i++) {
        int k = eoff + i;
        struct sc_mapping_entry *e = &mm->entries[idx[k]];
        float y = top + MGR_ROW_H * (float) (i + 1);
        bool sel = (k == ov->mgr_entry_sel);
        if (sel) {
            draw_panel(renderer, ex - 4, y - 2, (float) MGR_LIST_W, MGR_ROW_H,
                       ov->mgr_focus == 1 ? 60 : 40,
                       ov->mgr_focus == 1 ? 120 : 90, 220, 200);
        }
        snprintf(line, sizeof(line), "%s %-12s %s", e->type == SC_MAPPING_TYPE_SLIDE
                 ? "S" : "T", e->key_name, e->is_mouse ? "(mouse)" : "");
        draw_text(renderer, ex, y, line, 220, 220, 220);
    }

    // ---- detail column ----
    float dx0 = (float) (MGR_LIST_W * 2 + 12);
    draw_text(renderer, dx0, top, "Detail", 150, 220, 150);
    if (n > 0) {
        struct sc_mapping_entry *e = &mm->entries[idx[ov->mgr_entry_sel]];
        float y = top + MGR_ROW_H;
        snprintf(line, sizeof(line), "key: %s", e->key_name);
        draw_text(renderer, dx0, y, line, 255, 255, 255);
        y += MGR_ROW_H * 1.4f;

        snprintf(line, sizeof(line), "type: %s",
                 e->type == SC_MAPPING_TYPE_SLIDE ? "slide" : "touch");
        draw_text(renderer, dx0, y, line, 200, 200, 255);
        y += MGR_ROW_H * 1.4f;

        // editable fields: [1]x [2]y (slide: [3]dx [4]dy [5]duration)
        const char *labels[5] = { "1] x", "2] y", "3] dx", "4] dy", "5] duration" };
        char values[5][32];
        snprintf(values[0], sizeof(values[0]), "%.5f", e->nx);
        snprintf(values[1], sizeof(values[1]), "%.5f", e->ny);
        snprintf(values[2], sizeof(values[2]), "%.5f", e->ndx);
        snprintf(values[3], sizeof(values[3]), "%.5f", e->ndy);
        snprintf(values[4], sizeof(values[4]), "%d", e->duration_ms);
        bool show[5] = { true, true,
                         e->type == SC_MAPPING_TYPE_SLIDE,
                         e->type == SC_MAPPING_TYPE_SLIDE,
                         e->type == SC_MAPPING_TYPE_SLIDE };
        for (int k = 0; k < 5; k++) {
            if (!show[k]) continue;
            snprintf(line, sizeof(line), "%-12s : %s", labels[k], values[k]);
            bool editing = (ov->mgr_edit_field == k);
            draw_text(renderer, dx0, y, line, editing ? 255 : 210,
                      editing ? 230 : 210, 140);
            if (editing) {
                snprintf(line, sizeof(line), "   typing: %s", ov->mgr_edit_buf);
                draw_text(renderer, dx0, y + 10, line, 255, 255, 120);
                y += 10;
            }
            y += MGR_ROW_H * 1.4f;
        }

        snprintf(line, sizeof(line), "pid: %d", (int) e->pointer_id);
        draw_text(renderer, dx0, y, line, 150, 150, 150);
    } else {
        draw_text(renderer, dx0, top + MGR_ROW_H,
                  "No entry. Press N then a key.", 150, 150, 150);
    }

    // ---- footer ----
    float fy = (float) (win_h - MGR_FOOTER_H);
    draw_panel(renderer, 0, fy, (float) pw, (float) MGR_FOOTER_H,
               20, 20, 26, 235);
    const char *keys =
        "Tab focus  Up/Down select  N new  Del remove  T type  "
        "P pick point  O pick end  E enable  S save";
    draw_text(renderer, 8, fy + 4, keys, 170, 190, 220);
    const char *hint = ov->mgr_hint[0] ? ov->mgr_hint : "";
    draw_text(renderer, 8, fy + 18,
              ov->mgr_capture_key ? "Press the key to bind (Esc cancels)"
                                  : hint, 255, 220, 150);

    // mode marker on the video area
    snprintf(line, sizeof(line), "MODE: MANAGER (F5)");
    draw_text(renderer, (float) win_w - 160, 8, line, 120, 200, 255);
    if (ov->mgr_pick) {
        draw_text(renderer, (float) pw + 8, (float) win_h - 40,
                  ov->mgr_pick == 1 ? "Click a point on the video (x, y)"
                                    : "Click the slide end point", 255, 220, 120);
    }
}

bool
sc_overlay_manager_mouse(struct sc_overlay *ov, float mx, float my,
                         int win_w, int win_h) {
    if (ov->mode != SC_OVERLAY_MANAGER) return false;
    int pw = mgr_panel_width(win_w);
    if (mx >= (float) pw) return false; // video area: used for picking

    struct sc_mapping_manager *mm = ov->mm;
    if (!mm) return true;

    float top = (float) (MGR_HEADER_H + 8);
    float list_h = (float) win_h - top - MGR_FOOTER_H;
    if (my < top) return true; // header: consumed, no action

    int rows = (int) (list_h / MGR_ROW_H) - 1;
    if (rows < 1) rows = 1;
    float rel = my - top;
    int row = (int) (rel / MGR_ROW_H); // 0 = column title
    if (row <= 0) return true;

    if (mx < (float) MGR_LIST_W) {
        int toff = ov->mgr_tmpl_sel >= rows ? ov->mgr_tmpl_sel - rows + 1 : 0;
        int k = toff + row - 1;
        if (k >= 0 && k < ov->mgr_count) {
            ov->mgr_focus = 0;
            ov->mgr_tmpl_sel = k;
        }
        return true;
    }
    if (mx < (float) (MGR_LIST_W * 2)) {
        int idx[SC_MAPPING_MAX_ENTRIES];
        int n = collect_entries(mm, idx, SC_MAPPING_MAX_ENTRIES);
        int eoff = ov->mgr_entry_sel >= rows ? ov->mgr_entry_sel - rows + 1 : 0;
        int k = eoff + row - 1;
        if (k >= 0 && k < n) {
            ov->mgr_focus = 1;
            ov->mgr_entry_sel = k;
            ov->mgr_edit_field = -1;
        }
        return true;
    }
    return true;
}

// ============================================================
// Render entry point
// ============================================================

void
sc_overlay_render(struct sc_overlay *ov, SDL_Renderer *renderer,
                  int win_w, int win_h) {
    // ---- Template switch popup (F2) ----
    if (ov->mode == SC_OVERLAY_TEMPLATE_PICKER && ov->count > 0) {
        float popup_h = POPUP_PADDING * 2 + ov->count * ROW_HEIGHT;
        float px = (win_w - POPUP_WIDTH) / 2.0f;
        float py = (win_h - popup_h) / 2.0f;
        ov->popup_rect.x = px;
        ov->popup_rect.y = py;
        ov->popup_rect.w = POPUP_WIDTH;
        ov->popup_rect.h = popup_h;

        draw_panel(renderer, px, py, POPUP_WIDTH, popup_h, 20, 20, 20, 235);
        draw_border(renderer, px, py, POPUP_WIDTH, popup_h, 120, 120, 120, 255);
        draw_text(renderer, px + POPUP_PADDING, py + 4, "Select Template",
                  200, 200, 200);

        for (int i = 0; i < ov->count; i++) {
            float ry = py + POPUP_PADDING + i * ROW_HEIGHT;
            if (i == ov->hovered) {
                draw_panel(renderer, px + 4, ry, POPUP_WIDTH - 8,
                           ROW_HEIGHT - 4, 60, 120, 220, 230);
            }
            const char *name = ov->names[i] ? ov->names[i] : "";
            draw_text(renderer, px + POPUP_PADDING, ry + 12, name,
                      255, 255, 255);
        }
    }

    // ---- Design mode banner (F3) ----
    if (ov->mode == SC_OVERLAY_DESIGN) {
        float banner_h = 36;
        draw_panel(renderer, 0, 0, (float) win_w, banner_h, 200, 100, 0, 200);
        const char *hint = ov->design_hint[0] ? ov->design_hint
                            : "Design TOUCH: click a position, then a key";
        draw_text(renderer, 12, 12, hint, 255, 255, 255);
    }

    // ---- Manager panel (F5) ----
    if (ov->mode == SC_OVERLAY_MANAGER) {
        render_manager(ov, renderer, win_w, win_h);
    }

    // ---- Toggle indicator ----
    if (ov->show_toggle) {
        if (sc_tick_now() > ov->toggle_until) {
            ov->show_toggle = false;
        } else {
            float iw = 220, ih = 36;
            float ix = (win_w - iw) / 2.0f;
            float iy = (win_h - ih) / 2.0f;
            Uint8 r = ov->mapping_enabled ? 40 : 180;
            Uint8 g = ov->mapping_enabled ? 160 : 40;
            draw_panel(renderer, ix, iy, iw, ih, r, g, 60, 220);
            const char *txt = ov->mapping_enabled ? "Mapping: ON"
                                                  : "Mapping: OFF";
            draw_text(renderer, ix + 12, iy + 12, txt, 255, 255, 255);
        }
    }

    // ---- Status line (bottom) ----
    if (ov->status[0]) {
        if (sc_tick_now() > ov->status_until) {
            ov->status[0] = '\0';
        } else {
            float sh = 24;
            draw_panel(renderer, 0, (float) (win_h - sh), (float) win_w, sh,
                       0, 0, 0, 180);
            draw_text(renderer, 12, (float) (win_h - sh) + 6, ov->status,
                      200, 220, 255);
        }
    }
}
