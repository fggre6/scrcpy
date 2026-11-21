#include "common.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#include <sys/types.h>

#include "mapping_cond.h"
#include "mapping_manager.h"

// ============================================================
// Dependency conditions: reference grammar, states, flag registry
// ============================================================

static bool
flag_always_on(void *ctx) {
    (void) ctx;
    return true;
}

static bool
flag_always_off(void *ctx) {
    (void) ctx;
    return false;
}

static void
test_parse_key_ref(void) {
    char err[128];
    struct sc_mapping_cond c;

    // A plain name is a key condition.
    assert(sc_cond_parse_ref("A", &c, err, sizeof(err)));
    assert(c.kind == SC_COND_KIND_KEY);
    assert(c.nkeys == 1);
    assert(c.keys[0] == SDLK_A);
    assert(c.state == SC_COND_STATE_ON);

    // A mouse button is a key condition too.
    assert(sc_cond_parse_ref("MOUSE_RIGHT", &c, err, sizeof(err)));
    assert(c.kind == SC_COND_KIND_KEY);
    assert(c.nkeys == 1);
    assert(SC_MAPPING_KEYCODE_IS_MOUSE(c.keys[0]));
    assert(SC_MAPPING_KEYCODE_MOUSE_BUTTON(c.keys[0]) == 3);

    // A combo keeps every member: the condition holds when all are held.
    assert(sc_cond_parse_ref("lshift+a", &c, err, sizeof(err)));
    assert(c.kind == SC_COND_KIND_KEY);
    assert(c.nkeys == 2);
    assert(c.keys[0] == SDLK_LSHIFT);
    assert(c.keys[1] == SDLK_A);

    // The explicit "key:" namespace behaves exactly like the implicit one.
    assert(sc_cond_parse_ref("key:lshift+a", &c, err, sizeof(err)));
    assert(c.kind == SC_COND_KIND_KEY);
    assert(c.nkeys == 2);
    assert(c.keys[0] == SDLK_LSHIFT);

    // Unknown keys are rejected.
    assert(!sc_cond_parse_ref("NOT_A_KEY", &c, err, sizeof(err)));
    assert(!sc_cond_parse_ref("", &c, err, sizeof(err)));
    assert(!sc_cond_parse_ref("key:", &c, err, sizeof(err)));

    // A mouse button may not be part of a combo: it has no press state.
    assert(!sc_cond_parse_ref("MOUSE_LEFT+a", &c, err, sizeof(err)));

    // Too many members / duplicate members are rejected.
    assert(!sc_cond_parse_ref("a+b+c+d+e", &c, err, sizeof(err)));
    assert(!sc_cond_parse_ref("a+a", &c, err, sizeof(err)));
}

static void
test_parse_entry_ref(void) {
    char err[128];
    struct sc_mapping_cond c;

    assert(sc_cond_parse_ref("entry:VIRT", &c, err, sizeof(err)));
    assert(c.kind == SC_COND_KIND_ENTRY);
    assert(!strcmp(c.name, "VIRT"));

    // The name is trimmed and the prefix is case insensitive.
    assert(sc_cond_parse_ref("  Entry:  MOUSE_MOVE  ", &c, err, sizeof(err)));
    assert(c.kind == SC_COND_KIND_ENTRY);
    assert(!strcmp(c.name, "MOUSE_MOVE"));

    // An empty source name is rejected.
    assert(!sc_cond_parse_ref("entry:", &c, err, sizeof(err)));
    assert(!sc_cond_parse_ref("entry:   ", &c, err, sizeof(err)));
}

static void
test_parse_flag_ref(void) {
    char err[128];
    struct sc_mapping_cond c;

    // The registry is empty here: the name is accepted but not resolved.
    assert(!sc_cond_flag_registry_ready());
    assert(sc_cond_parse_ref("flag:look_mode", &c, err, sizeof(err)));
    assert(c.kind == SC_COND_KIND_FLAG);
    assert(!strcmp(c.name, "look_mode"));

    // Once the registry is up, an unknown flag is rejected.
    assert(sc_cond_flag_register("look_mode", "test flag", flag_always_on));
    assert(sc_cond_flag_registry_ready());
    assert(sc_cond_parse_ref("flag:look_mode", &c, err, sizeof(err)));
    assert(c.kind == SC_COND_KIND_FLAG);
    assert(!sc_cond_parse_ref("flag:nope", &c, err, sizeof(err)));
    assert(!sc_cond_parse_ref("flag:", &c, err, sizeof(err)));

    // The registry is case insensitive and rejects duplicates.
    assert(sc_cond_flag_find("LOOK_MODE"));
    assert(!sc_cond_flag_register("look_mode", "again", flag_always_off));
    assert(!sc_cond_flag_register("", "empty", flag_always_off));

    // The registered getter is the one that is called.
    assert(sc_cond_flag_find("look_mode")->fn(NULL) == true);
    assert(sc_cond_flag_count() >= 1);
    assert(sc_cond_flag_at(0));
    assert(!sc_cond_flag_at(-1));
    assert(!sc_cond_flag_at(sc_cond_flag_count()));
}

static void
test_parse_state(void) {
    enum sc_mapping_cond_state st;
    char err[128];

    assert(sc_cond_parse_state("pressed", &st, err, sizeof(err)));
    assert(st == SC_COND_STATE_ON);
    assert(sc_cond_parse_state("PRESSED", &st, err, sizeof(err)));
    assert(st == SC_COND_STATE_ON);
    assert(sc_cond_parse_state("released", &st, err, sizeof(err)));
    assert(st == SC_COND_STATE_OFF);

    // The synonyms of the other source kinds are accepted too.
    assert(sc_cond_parse_state("active", &st, err, sizeof(err)));
    assert(st == SC_COND_STATE_ON);
    assert(sc_cond_parse_state("inactive", &st, err, sizeof(err)));
    assert(st == SC_COND_STATE_OFF);
    assert(sc_cond_parse_state("on", &st, err, sizeof(err)));
    assert(st == SC_COND_STATE_ON);
    assert(sc_cond_parse_state("off", &st, err, sizeof(err)));
    assert(st == SC_COND_STATE_OFF);
    assert(sc_cond_parse_state("enabled", &st, err, sizeof(err)));
    assert(st == SC_COND_STATE_ON);
    assert(sc_cond_parse_state("disabled", &st, err, sizeof(err)));
    assert(st == SC_COND_STATE_OFF);

    // Unknown states are rejected.
    assert(!sc_cond_parse_state("maybe", &st, err, sizeof(err)));
    assert(!sc_cond_parse_state("", &st, err, sizeof(err)));
}

static void
test_render_names(void) {
    char buf[128];
    struct sc_mapping_cond c;

    // The mouse names are produced by scrcpy itself, so they are exact.
    assert(sc_cond_parse_ref("MOUSE_LEFT", &c, buf, sizeof(buf)));
    assert(!strcmp(sc_cond_to_name(&c, buf, sizeof(buf)), "MOUSE_LEFT"));
    assert(!strcmp(sc_cond_state_name(&c), "pressed"));

    // The keyboard names come from SDL, so only the round-trip is checked
    // (see test_round_trip).
    assert(sc_cond_parse_ref("lshift+a", &c, buf, sizeof(buf)));
    assert(strchr(sc_cond_to_name(&c, buf, sizeof(buf)), '+'));

    assert(sc_cond_parse_ref("entry:VIRT", &c, buf, sizeof(buf)));
    assert(!strcmp(sc_cond_to_name(&c, buf, sizeof(buf)), "entry:VIRT"));
    assert(!strcmp(sc_cond_state_name(&c), "active"));

    assert(sc_cond_parse_ref("flag:look_mode", &c, buf, sizeof(buf)));
    assert(!strcmp(sc_cond_to_name(&c, buf, sizeof(buf)), "flag:look_mode"));
    assert(!strcmp(sc_cond_state_name(&c), "on"));

    // The state word follows the source kind.
    c.state = SC_COND_STATE_OFF;
    assert(!strcmp(sc_cond_state_name(&c), "off"));
    assert(sc_cond_parse_ref("MOUSE_RIGHT", &c, buf, sizeof(buf)));
    c.state = SC_COND_STATE_OFF;
    assert(!strcmp(sc_cond_state_name(&c), "released"));
}

static void
test_round_trip(void) {
    // A reference must survive a parse -> render -> parse cycle.
    char err[128];
    char buf[128];
    struct sc_mapping_cond a, b;

    static const char *refs[] = {
        "A",
        "MOUSE_LEFT",
        "lshift+a",
        "entry:VIRT",
        "flag:look_mode",
    };

    for (size_t i = 0; i < sizeof(refs) / sizeof(refs[0]); ++i) {
        assert(sc_cond_parse_ref(refs[i], &a, err, sizeof(err)));
        sc_cond_to_name(&a, buf, sizeof(buf));
        assert(sc_cond_parse_ref(buf, &b, err, sizeof(err)));
        assert(a.kind == b.kind);
        assert(a.nkeys == b.nkeys);
        for (int k = 0; k < a.nkeys; k++) {
            assert(a.keys[k] == b.keys[k]);
        }
        assert(!strcmp(a.name, b.name));
    }
}

static void
test_validate_ref_api(void) {
    char err[128];

    assert(sc_mapping_validate_cond_ref("lshift", err, sizeof(err)));
    assert(sc_mapping_validate_cond_ref("entry:VIRT", err, sizeof(err)));
    assert(sc_mapping_validate_cond_ref("flag:look_mode", err, sizeof(err)));
    assert(!sc_mapping_validate_cond_ref("NOT_A_KEY", err, sizeof(err)));

    // The key expression validator keeps its own rules.
    assert(sc_mapping_validate_key_expr("a", err, sizeof(err)));
    assert(sc_mapping_validate_key_expr("W+A", err, sizeof(err)));
    assert(!sc_mapping_validate_key_expr("NOT_A_KEY", err, sizeof(err)));
}

static void
test_init_key_cond(void) {
    struct sc_mapping_cond c;
    sc_cond_init_key(&c, SDLK_SPACE, SC_COND_STATE_OFF);
    assert(c.kind == SC_COND_KIND_KEY);
    assert(c.nkeys == 1);
    assert(c.keys[0] == SDLK_SPACE);
    assert(c.state == SC_COND_STATE_OFF);
}

// ============================================================
// End-to-end: template load -> save -> load
// ============================================================

// The manager resolves its templates directory relative to the executable, so
// the test writes its template next to the test binary. The returned path
// ends with a separator (SDL guarantees it).
static char *
base_dir(void) {
    char *base = sc_mapping_get_base_dir();
    assert(base);
    return base;
}

static void
write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    assert(f);
    size_t len = strlen(text);
    assert(fwrite(text, 1, len, f) == len);
    fclose(f);
}

static char *
read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    assert(size >= 0);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t) size + 1);
    assert(text);
    assert(fread(text, 1, (size_t) size, f) == (size_t) size);
    text[size] = '\0';
    fclose(f);
    return text;
}

static struct sc_mapping_entry *
entry_by_name(struct sc_mapping_manager *mm, const char *name) {
    for (int i = 0; i < SC_MAPPING_MAX_ENTRIES; i++) {
        if (mm->entries[i].key_name
                && SDL_strcasecmp(mm->entries[i].key_name, name) == 0) {
            return &mm->entries[i];
        }
    }
    return NULL;
}

static void
test_template_round_trip(void) {
    char *base = base_dir();
    char dir[512];
    snprintf(dir, sizeof(dir), "%stemplates", base);
    // The templates directory usually exists in a checkout; create it when it
    // does not (the test binary lives in the build directory).
    mkdir(dir, 0755);

    char path[600];
    snprintf(path, sizeof(path), "%s/condtest.json", dir);

    // A template that uses every kind of dependency condition.
    write_file(path,
        "{ \"name\": \"condtest\", \"mappings\": [\n"
        "  { \"key\": \"F9\", \"virtual\": true },\n"
        "  { \"key\": \"F1\", \"x\": 0.5, \"y\": 0.5,\n"
        "    \"depends_on\": \"entry:F9\", \"depends_state\": \"active\" },\n"
        "  { \"key\": \"F2\", \"x\": 0.5, \"y\": 0.5,\n"
        "    \"depends_on\": [\"lshift\", \"flag:look_mode\"],\n"
        "    \"depends_state\": [\"pressed\", \"on\"] },\n"
        "  { \"key\": \"F3\", \"x\": 0.5, \"y\": 0.5,\n"
        "    \"depends_on\": \"MOUSE_RIGHT\", \"depends_state\": \"released\" }\n"
        "] }\n");

    struct sc_mapping_manager mm;
    memset(&mm, 0, sizeof(mm));
    assert(sc_mapping_manager_init(&mm));
    assert(sc_mapping_manager_switch(&mm, "condtest"));
    assert(mm.last_load.fail == 0);

    // The entry dependency resolved to the referenced entry.
    struct sc_mapping_entry *e = entry_by_name(&mm, "F1");
    assert(e);
    assert(e->ndeps == 1);
    assert(e->deps[0].kind == SC_COND_KIND_ENTRY);
    assert(!strcmp(e->deps[0].name, "F9"));
    assert(e->deps[0].state == SC_COND_STATE_ON);

    // The array form kept both conditions, with their own states.
    e = entry_by_name(&mm, "F2");
    assert(e);
    assert(e->ndeps == 2);
    assert(e->deps[0].kind == SC_COND_KIND_KEY);
    assert(e->deps[0].keys[0] == SDLK_LSHIFT);
    assert(e->deps[0].state == SC_COND_STATE_ON);
    assert(e->deps[1].kind == SC_COND_KIND_FLAG);
    assert(!strcmp(e->deps[1].name, "look_mode"));
    assert(e->deps[1].state == SC_COND_STATE_ON);

    // A "released" key condition.
    e = entry_by_name(&mm, "F3");
    assert(e);
    assert(e->ndeps == 1);
    assert(e->deps[0].kind == SC_COND_KIND_KEY);
    assert(SC_MAPPING_KEYCODE_IS_MOUSE(e->deps[0].keys[0]));
    assert(e->deps[0].state == SC_COND_STATE_OFF);

    // Save and reload: the semantics must be identical.
    assert(sc_mapping_manager_save_active_force(&mm));
    struct sc_mapping_manager mm2;
    memset(&mm2, 0, sizeof(mm2));
    assert(sc_mapping_manager_init(&mm2));
    assert(sc_mapping_manager_switch(&mm2, "condtest"));
    assert(mm2.last_load.fail == 0);

    e = entry_by_name(&mm2, "F1");
    assert(e);
    assert(e->ndeps == 1);
    assert(e->deps[0].kind == SC_COND_KIND_ENTRY);
    assert(!strcmp(e->deps[0].name, "F9"));
    e = entry_by_name(&mm2, "F2");
    assert(e);
    assert(e->ndeps == 2);
    assert(e->deps[0].kind == SC_COND_KIND_KEY);
    assert(e->deps[1].kind == SC_COND_KIND_FLAG);
    assert(!strcmp(e->deps[1].name, "look_mode"));
    e = entry_by_name(&mm2, "F3");
    assert(e);
    assert(e->ndeps == 1);
    assert(e->deps[0].state == SC_COND_STATE_OFF);

    // The saved file keeps the canonical form.
    char *text = read_file(path);
    assert(strstr(text, "\"entry:F9\""));
    assert(strstr(text, "\"flag:look_mode\""));
    free(text);

    sc_mapping_manager_destroy(&mm2);
    sc_mapping_manager_destroy(&mm);
    SDL_free(base);
}

static void
test_template_rejects_bad_deps(void) {
    char *base = base_dir();
    char dir[512];
    snprintf(dir, sizeof(dir), "%stemplates", base);
    mkdir(dir, 0755);

    char path[600];
    snprintf(path, sizeof(path), "%s/condbad.json", dir);

    // An unknown entry dependency, a self dependency and a cycle must all be
    // rejected, while the valid entry survives.
    write_file(path,
        "{ \"name\": \"condbad\", \"mappings\": [\n"
        "  { \"key\": \"F1\", \"x\": 0.5, \"y\": 0.5, \"depends_on\": \"entry:NOPE\" },\n"
        "  { \"key\": \"F2\", \"x\": 0.5, \"y\": 0.5, \"depends_on\": \"entry:F2\" },\n"
        "  { \"key\": \"F3\", \"x\": 0.5, \"y\": 0.5, \"depends_on\": \"entry:F4\" },\n"
        "  { \"key\": \"F4\", \"x\": 0.5, \"y\": 0.5, \"depends_on\": \"entry:F3\" },\n"
        "  { \"key\": \"F5\", \"x\": 0.5, \"y\": 0.5, \"depends_on\": \"lshift\" }\n"
        "] }\n");

    struct sc_mapping_manager mm;
    memset(&mm, 0, sizeof(mm));
    assert(sc_mapping_manager_init(&mm));
    assert(sc_mapping_manager_switch(&mm, "condbad"));

    // F1 (unknown target), F2 (self), F3 and F4 (cycle) are rejected; F5 is
    // kept.
    assert(mm.last_load.fail == 4);
    assert(!entry_by_name(&mm, "F1"));
    assert(!entry_by_name(&mm, "F2"));
    assert(!entry_by_name(&mm, "F3"));
    assert(!entry_by_name(&mm, "F4"));
    struct sc_mapping_entry *e = entry_by_name(&mm, "F5");
    assert(e);
    assert(e->ndeps == 1);

    sc_mapping_manager_destroy(&mm);
    SDL_free(base);
}

int
main(int argc, char *argv[]) {
    (void) argc;
    (void) argv;
    test_parse_key_ref();
    test_parse_entry_ref();
    test_parse_flag_ref();
    test_parse_state();
    test_render_names();
    test_round_trip();
    test_validate_ref_api();
    test_init_key_cond();
    test_template_round_trip();
    test_template_rejects_bad_deps();
    printf("test_mapping_cond: OK\n");
    return 0;
}
