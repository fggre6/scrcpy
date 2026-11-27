#ifndef SC_INPUTMANAGER_H
#define SC_INPUTMANAGER_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>

#include "controller.h"
#include "file_pusher.h"
#include "options.h"
#include "trait/gamepad_processor.h"
#include "trait/key_processor.h"
#include "trait/mouse_processor.h"

#include "util/tick.h"

// Minimum interval between injected mouse-motion MOVEs. A MOVE is droppable
// and the control queue holds 60 messages, so motion is throttled instead of
// flooding: at most one MOVE every 4 ms (250 Hz), with deltas in between
// accumulated. Shared with the frame-time diagnostic in screen.c.
#define MOTION_MIN_INTERVAL_MS 4

// Concurrent hold-to-slide instances (each holds its own pointer id)
#define SC_MAX_SLIDES 4

// A running slide. It stores *values* only (never an entry pointer), so a
// hot-reload that replaces the entry table cannot dangle underneath it.
struct sc_slide_run {
    bool active;
    int32_t pointer_id;
    double nx, ny;   // start point (normalized)
    double ndx, ndy; // delta (normalized, may be negative)
    double cur_nx, cur_ny; // last injected position
    int steps;
    int step_ms;
    int step;        // last emitted step index
    sc_tick start;   // start of the current segment
};

// A running mouse-motion swipe. Like the slide run it keeps value copies only.
// The pointer id is the motion entry's own id, so it never fights a slide or a
// key entry for the same server-side pointer slot.
struct sc_motion_run {
    bool active;
    int32_t pointer_id;
    double anchor_nx, anchor_ny; // re-anchor point after a boundary wrap
    double cur_nx, cur_ny;       // last injected position
    double sensitivity;
    double pend_nx, pend_ny;     // deltas not yet injected (rate limiting)
    sc_tick last_inject;
    // Relative mouse mode is engaged while the finger is down so the OS
    // cursor is hidden and keeps producing xrel past the screen edge. It is
    // released the moment the finger lifts, so absolute input works again.
    bool capture_active;
    // The first motion event after (re)entering relative mode carries the
    // recenter jump (old position -> window center); drop that one delta.
    bool discard_next;
};

struct sc_input_manager {
    struct sc_controller *controller;
    struct sc_file_pusher *fp;
    struct sc_screen *screen;

    struct sc_key_processor *kp;
    struct sc_mouse_processor *mp;
    struct sc_gamepad_processor *gp;

    bool camera;

    struct sc_mouse_bindings mouse_bindings;
    bool legacy_paste;
    bool clipboard_autosync;

    uint16_t sdl_shortcut_mods;

    bool vfinger_down;
    bool vfinger_invert_x;
    bool vfinger_invert_y;

    uint8_t mouse_buttons_state; // OR of enum sc_mouse_button values

    // Tracks the number of identical consecutive shortcut key down events.
    // Not to be confused with event->repeat, which counts the number of
    // system-generated repeated key presses.
    unsigned key_repeat;
    SDL_Keycode last_keycode;
    uint16_t last_mod;

    uint64_t next_sequence; // used for request acknowledgements

    bool disconnected;

    // ---- Custom key-mapping state ----
    bool g_pressed[256];       // index = SDL keycode & 0xFF
    bool left_click_locked;
    bool left_click_locked_down;
    // Mouse buttons tracked independently of forwarding/consumption, so a
    // button bound in a template is still visible as a mapping trigger and as
    // a dependency source. Index = SDL button number (1..5).
    bool map_mouse_down[6];

    // Design mode
    bool design_mode;
    // Switch popup open
    bool switch_open;

    // Running slides (advanced from the event loop, no thread detached)
    struct sc_slide_run slides[SC_MAX_SLIDES];

    // Running mouse-motion swipe (at most one per template)
    struct sc_motion_run motion;
};

struct sc_input_manager_params {
    struct sc_controller *controller;
    struct sc_file_pusher *fp;
    struct sc_screen *screen;
    struct sc_key_processor *kp;
    struct sc_mouse_processor *mp;
    struct sc_gamepad_processor *gp;
    bool camera;

    struct sc_mouse_bindings mouse_bindings;
    bool legacy_paste;
    bool clipboard_autosync;
    uint8_t shortcut_mods; // OR of enum sc_shortcut_mod values
};

void
sc_input_manager_init(struct sc_input_manager *im,
                      const struct sc_input_manager_params *params);

void
sc_input_manager_handle_event(struct sc_input_manager *im,
                              const SDL_Event *event);

// Advance running slides. Called from the event loop (no thread is detached);
// also stops them when paused/disconnected/disabled.
void
sc_input_manager_tick(struct sc_input_manager *im);

#endif
