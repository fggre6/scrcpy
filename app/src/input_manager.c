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
#include "screen.h"
#include "shortcut_mod.h"
#include "util/log.h"
#include "util/sdl.h"

// ==================== 自定义映射（从文件加载） ====================

struct sc_custom_mapping {
    SDL_Keycode keycode;
    int32_t x;
    int32_t y;
    uint64_t pointer_id;
    bool down;
};
#define MAX_COMBO_KEYS 4

struct sc_custom_combo {
    SDL_Keycode keys[MAX_COMBO_KEYS];
    int nkeys;
    int32_t x;
    int32_t y;
    uint64_t pointer_id;
    bool active;
};

static struct sc_custom_combo *g_custom_combos = NULL;
static int g_custom_combo_count = 0;
static int g_custom_combo_capacity = 0;

// 当前按下的所有映射键
static bool g_pressed[256];  // 索引为 SDL keycode & 0xFF，简化处理

static struct sc_custom_mapping *g_custom_mappings = NULL;
static int g_custom_mapping_count = 0;
static int g_custom_mapping_capacity = 0;

// 特殊鼠标坐标
static int32_t g_right_click_x = 1953;
static int32_t g_right_click_y = 826;
static int32_t g_left_click_locked_x = 1953;
static int32_t g_left_click_locked_y = 826;

// 左 Alt 切换左键锁定状态
static bool g_left_click_locked = false;
static bool g_left_click_locked_down = false;
static bool g_right_click_down = false;

static SDL_Keycode
parse_key_name(const char *name) {
    char buf[64];
    strncpy(buf, name, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    // trim
    char *start = buf;
    while (*start && isspace((unsigned char)*start)) start++;
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';
    if (*start == '\0') return SDLK_UNKNOWN;

    // 自定义名称（全 ASCII，避免编码问题）
    if (SDL_strcasecmp(start, "space") == 0) return SDLK_SPACE;
    if (SDL_strcasecmp(start, "lshift") == 0 ||
        SDL_strcasecmp(start, "left shift") == 0 ||
        SDL_strcasecmp(start, "shift") == 0) return SDLK_LSHIFT;
    if (SDL_strcasecmp(start, "rshift") == 0 ||
        SDL_strcasecmp(start, "right shift") == 0) return SDLK_RSHIFT;
    if (SDL_strcasecmp(start, "esc") == 0 ||
        SDL_strcasecmp(start, "escape") == 0) return SDLK_ESCAPE;

    // 单字母 / 数字
    if (strlen(start) == 1) {
        char c = toupper((unsigned char)start[0]);
        if (c >= 'A' && c <= 'Z') return SDLK_A + (c - 'A');
        if (c >= '0' && c <= '9') return SDLK_0 + (c - '0');
    }

    // 尝试 SDL 原生名称
    SDL_Keycode kc = SDL_GetKeyFromName(start);
    if (kc != SDLK_UNKNOWN) return kc;

    // 尝试大写
    char upper[64];
    for (int i = 0; start[i]; i++) upper[i] = toupper((unsigned char)start[i]);
    upper[strlen(start)] = '\0';
    kc = SDL_GetKeyFromName(upper);
    if (kc != SDLK_UNKNOWN) return kc;

    return SDLK_UNKNOWN;
}

static void
add_custom_mapping(SDL_Keycode kc, int32_t x, int32_t y) {
    if (g_custom_mapping_count >= g_custom_mapping_capacity) {
        int new_cap = g_custom_mapping_capacity ? g_custom_mapping_capacity * 2 : 16;
        struct sc_custom_mapping *new_arr = realloc(g_custom_mappings, new_cap * sizeof(*new_arr));
        if (!new_arr) return;
        g_custom_mappings = new_arr;
        g_custom_mapping_capacity = new_cap;
    }
    g_custom_mappings[g_custom_mapping_count].keycode = kc;
    g_custom_mappings[g_custom_mapping_count].x = x;
    g_custom_mappings[g_custom_mapping_count].y = y;
    g_custom_mappings[g_custom_mapping_count].pointer_id = (uint64_t)-(10 + g_custom_mapping_count);
    g_custom_mappings[g_custom_mapping_count].down = false;
    g_custom_mapping_count++;
}
static void
add_custom_combo(SDL_Keycode *keys, int nkeys, int32_t x, int32_t y) {
    if (g_custom_combo_count >= g_custom_combo_capacity) {
        int new_cap = g_custom_combo_capacity ? g_custom_combo_capacity * 2 : 8;
        struct sc_custom_combo *new_arr = realloc(g_custom_combos, new_cap * sizeof(*new_arr));
        if (!new_arr) return;
        g_custom_combos = new_arr;
        g_custom_combo_capacity = new_cap;
    }
    struct sc_custom_combo *c = &g_custom_combos[g_custom_combo_count];
    memcpy(c->keys, keys, nkeys * sizeof(SDL_Keycode));
    c->nkeys = nkeys;
    c->x = x;
    c->y = y;
    c->pointer_id = (uint64_t)-(100 + g_custom_combo_count);
    c->active = false;
    g_custom_combo_count++;
}

// 判断某个键是否被任何 combo 覆盖
static bool
key_in_combo(SDL_Keycode kc) {
    for (int i = 0; i < g_custom_combo_count; ++i) {
        for (int j = 0; j < g_custom_combos[i].nkeys; ++j) {
            if (g_custom_combos[i].keys[j] == kc) return true;
        }
    }
    return false;
}
// 前向声明
static struct sc_custom_mapping *
find_mapping(SDL_Keycode keycode);

static bool
inject_touch_at(struct sc_input_manager *im, enum android_motionevent_action action,
                int32_t x, int32_t y, uint64_t pointer_id);
// 检查所有 combo，匹配成功的激活，不再匹配的释放
static void
update_combos(struct sc_input_manager *im) {
    for (int i = 0; i < g_custom_combo_count; ++i) {
        struct sc_custom_combo *c = &g_custom_combos[i];
        bool match = true;
        for (int j = 0; j < c->nkeys; ++j) {
            if (!g_pressed[c->keys[j] & 0xFF]) {
                match = false;
                break;
            }
        }

        if (match && !c->active) {
            // 组合键激活：
            // 先撤销组合键里所有键的单键 DOWN，避免同时按下两个位置
            for (int j = 0; j < c->nkeys; ++j) {
                struct sc_custom_mapping *m = find_mapping(c->keys[j]);
                if (m && m->down) {
                    inject_touch_at(im, AMOTION_EVENT_ACTION_UP,
                                    m->x, m->y, m->pointer_id);
                    m->down = false;
                }
            }
            // 再发送组合键 DOWN
            c->active = true;
            inject_touch_at(im, AMOTION_EVENT_ACTION_DOWN,
                            c->x, c->y, c->pointer_id);
            LOGI("Combo down: %d keys -> (%d, %d)", c->nkeys, c->x, c->y);

        } else if (!match && c->active) {
            // 组合键停用：
            // 先发送组合键 UP
            c->active = false;
            inject_touch_at(im, AMOTION_EVENT_ACTION_UP,
                            c->x, c->y, c->pointer_id);
            LOGI("Combo up: %d keys", c->nkeys);

            // 对于组合键里仍然按着的键，重新发送单键 DOWN
            for (int j = 0; j < c->nkeys; ++j) {
                if (!g_pressed[c->keys[j] & 0xFF]) continue;
                struct sc_custom_mapping *m = find_mapping(c->keys[j]);
                if (m && !m->down) {
                    inject_touch_at(im, AMOTION_EVENT_ACTION_DOWN,
                                    m->x, m->y, m->pointer_id);
                    m->down = true;
                }
            }
        }
    }
}

static void
load_mappings(void) {
    g_custom_mapping_count = 0;
    const char *filename = "scrcpy_mappings.txt";
    FILE *f = fopen(filename, "r");
    if (!f) {
        char *base = SDL_GetBasePath();
        if (base) {
            char path[512];
            snprintf(path, sizeof(path), "%s%s", base, filename);
            SDL_free(base);
            f = fopen(path, "r");
        }
    }
    if (!f) {
        LOGW("Could not open mappings file: %s", filename);
        return;
    }

    char line[256];
    bool first_line = true;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;

        // 跳过 UTF-8 BOM（只在第一行）
        if (first_line) {
            first_line = false;
            if ((unsigned char)p[0] == 0xEF &&
                (unsigned char)p[1] == 0xBB &&
                (unsigned char)p[2] == 0xBF) {
                p += 3;
            }
        }

        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;

        char *colon = strchr(p, ':');
        if (!colon) continue;
        *colon = '\0';
        char *keyname = p;

        char *end = keyname + strlen(keyname);
        while (end > keyname && isspace((unsigned char)*(end - 1))) end--;
        *end = '\0';

        int x, y;
        if (sscanf(colon + 1, "%d , %d", &x, &y) != 2 &&
            sscanf(colon + 1, "%d %d", &x, &y) != 2) {
            continue;
        }

        if (SDL_strcasecmp(keyname, "MOUSE_RIGHT") == 0 ||
            SDL_strcasecmp(keyname, "RIGHT_CLICK") == 0) {
            g_right_click_x = x;
            g_right_click_y = y;
            continue;
        }
        if (SDL_strcasecmp(keyname, "MOUSE_LEFT_LOCKED") == 0 ||
            SDL_strcasecmp(keyname, "LEFT_CLICK_LOCKED") == 0) {
            g_left_click_locked_x = x;
            g_left_click_locked_y = y;
            continue;
        }

        // ==================== 组合键识别 ====================
        if (strchr(keyname, '+')) {
            SDL_Keycode keys[MAX_COMBO_KEYS];
            int nkeys = 0;
            char tmp[128];
            strncpy(tmp, keyname, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';

            char *save = NULL;
            char *tok = strtok_r(tmp, "+", &save);
            while (tok && nkeys < MAX_COMBO_KEYS) {
                while (*tok == ' ' || *tok == '\t') tok++;
                char *e = tok + strlen(tok);
                while (e > tok && isspace((unsigned char)*(e - 1))) e--;
                *e = '\0';

                SDL_Keycode kc = parse_key_name(tok);
                if (kc == SDLK_UNKNOWN) {
                    LOGW("Unknown combo key: %s", tok);
                    nkeys = 0;
                    break;
                }
                keys[nkeys++] = kc;
                tok = strtok_r(NULL, "+", &save);
            }
            if (nkeys >= 2) {
                add_custom_combo(keys, nkeys, x, y);
                LOGI("Loaded combo: %s -> (%d, %d)", keyname, x, y);
            } else {
                LOGW("Invalid combo: %s", keyname);
            }
            continue;
        }
        // ==================== 组合键识别结束 ====================

        SDL_Keycode kc = parse_key_name(keyname);
        if (kc != SDLK_UNKNOWN) {
            add_custom_mapping(kc, x, y);
        } else {
            LOGW("Unknown key name: %s", keyname);
        }
    }
    fclose(f);
    LOGI("Loaded %d custom mappings, %d combos",
         g_custom_mapping_count, g_custom_combo_count);
}

static struct sc_custom_mapping *
find_mapping(SDL_Keycode keycode) {
    for (int i = 0; i < g_custom_mapping_count; ++i) {
        if (g_custom_mappings[i].keycode == keycode) {
            return &g_custom_mappings[i];
        }
    }
    return NULL;
}

// 直接发送手机坐标的触摸事件
static bool
inject_touch_at(struct sc_input_manager *im, enum android_motionevent_action action,
                int32_t x, int32_t y, uint64_t pointer_id) {
    bool up = action == AMOTION_EVENT_ACTION_UP;

    struct sc_control_msg msg;
    msg.type = SC_CONTROL_MSG_TYPE_INJECT_TOUCH_EVENT;
    msg.inject_touch_event.action = action;
    msg.inject_touch_event.position.screen_size = im->screen->frame_size;
    msg.inject_touch_event.position.point.x = x;
    msg.inject_touch_event.position.point.y = y;
    msg.inject_touch_event.pointer_id = pointer_id;
    msg.inject_touch_event.pressure = up ? 0.0f : 1.0f;
    msg.inject_touch_event.action_button = 0;
    msg.inject_touch_event.buttons = 0;

    if (!sc_controller_push_msg(im->controller, &msg)) {
        LOGW("Could not request 'inject custom touch event'");
        return false;
    }
    return true;
}
// ==================== 自定义映射结束 ====================


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

    load_mappings();
}

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

    // ==================== 自定义拦截 ====================
    if (control && !paused && !disconnected && !im->camera) {
        // 左 Alt 切换左键锁定
        if (sdl_keycode == SDLK_LALT && down && !repeat && !ctrl && !shift) {
            g_left_click_locked = !g_left_click_locked;
            g_left_click_locked_down = false;
            LOGI("Left click lock: %s", g_left_click_locked ? "ON" : "OFF");
            return;
        }

        // Escape 映射
        if (sdl_keycode == SDLK_ESCAPE) {
            struct sc_custom_mapping *m = find_mapping(SDLK_ESCAPE);
            if (m) {
                if (down && repeat) return;
                if (down && m->down) return;
                if (!down && !m->down) return;
                enum android_motionevent_action action = down
                    ? AMOTION_EVENT_ACTION_DOWN : AMOTION_EVENT_ACTION_UP;
                inject_touch_at(im, action, m->x, m->y, m->pointer_id);
                m->down = down;
                return;
            } else {
                enum android_motionevent_action action = down
                    ? AMOTION_EVENT_ACTION_DOWN : AMOTION_EVENT_ACTION_UP;
                inject_touch_at(im, action, 1776, 155, (uint64_t)-23);
                return;
            }
        }

                // 记录按键状态
        bool is_mapping_key = find_mapping(sdl_keycode) != NULL;
        bool is_combo_key = key_in_combo(sdl_keycode);
        if (is_mapping_key || is_combo_key) {
            if (down && repeat) return;  // 忽略键盘自动重复

            // 更新全局按下状态
            if (down) {
                g_pressed[sdl_keycode & 0xFF] = true;
            } else {
                g_pressed[sdl_keycode & 0xFF] = false;
            }

            // 优先检查 combo：先更新 combo 状态
            if (g_custom_combo_count > 0) {
                update_combos(im);
            }

            // 单键处理：如果该键没有被任何 combo 覆盖，或没有活跃 combo 覆盖它
            struct sc_custom_mapping *m = find_mapping(sdl_keycode);
            if (m) {
                // 如果该键属于某个已激活的 combo，则跳过单键逻辑
                bool covered_by_active_combo = false;
                for (int i = 0; i < g_custom_combo_count; ++i) {
                    struct sc_custom_combo *c = &g_custom_combos[i];
                    if (!c->active) continue;
                    for (int j = 0; j < c->nkeys; ++j) {
                        if (c->keys[j] == sdl_keycode) {
                            covered_by_active_combo = true;
                            break;
                        }
                    }
                    if (covered_by_active_combo) break;
                }

                if (!covered_by_active_combo) {
                    if (down && m->down) return;
                    if (!down && !m->down) return;
                    enum android_motionevent_action action = down
                        ? AMOTION_EVENT_ACTION_DOWN : AMOTION_EVENT_ACTION_UP;
                    inject_touch_at(im, action, m->x, m->y, m->pointer_id);
                    m->down = down;
                } else if (!down && m->down) {
                    // 从 combo 覆盖切换到不覆盖时，需要补发单键的 UP
                    m->down = false;
                    inject_touch_at(im, AMOTION_EVENT_ACTION_UP, m->x, m->y, m->pointer_id);
                }
            }
            return;
        }
    }
    // ==================== 自定义拦截结束 ====================

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

    // ==================== 自定义鼠标拦截 ====================
    if (control && !paused && !im->camera) {
        // 左键锁定
        if (event->button == SDL_BUTTON_LEFT && g_left_click_locked) {
            if (down == g_left_click_locked_down) return;
            g_left_click_locked_down = down;
            enum android_motionevent_action action = down
                ? AMOTION_EVENT_ACTION_DOWN : AMOTION_EVENT_ACTION_UP;
            inject_touch_at(im, action, g_left_click_locked_x, g_left_click_locked_y, (uint64_t)-25);
            return;
        }

        // 右键固定映射
        if (event->button == SDL_BUTTON_RIGHT) {
            if (down == g_right_click_down) return;
            g_right_click_down = down;
            enum android_motionevent_action action = down
                ? AMOTION_EVENT_ACTION_DOWN : AMOTION_EVENT_ACTION_UP;
            inject_touch_at(im, action, g_right_click_x, g_right_click_y, (uint64_t)-24);
            return;
        }
    }
    // ==================== 自定义鼠标拦截结束 ====================

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

    float mouse_x, mouse_y;
    uint32_t buttons = SDL_GetMouseState(&mouse_x, &mouse_y);
    (void)buttons;

    struct sc_mouse_scroll_event evt = {
        .position = sc_input_manager_get_position(im, mouse_x, mouse_y),
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
    }
}
