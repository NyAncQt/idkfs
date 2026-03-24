/*
 * idkfs_config.c — JS config layer for idkfs using QuickJS
 *
 * Reads idkfs.config.js at mount time and applies:
 *   - feature flags (disable journaling, checksums, etc)
 *   - speed tier rules (which file patterns go to FAST/NORMAL/SLOW)
 *   - sorting algorithm preference
 *
 * Include this AFTER idkfs_core.c, BEFORE idkfs_persist.c / fuse layer.
 *
 * Compile flags needed:
 *   -I/usr/local/include/quickjs -L/usr/local/lib/quickjs -lquickjs -lm -lpthread -ldl
 */

#include <quickjs/quickjs.h>
#include <quickjs/quickjs-libc.h>
#include <fnmatch.h>

/* ============================================================
 * DEFAULT CONFIG — used if no idkfs.config.js found
 * ============================================================ */

#define IDKFS_MAX_TIER_RULES 64

typedef struct {
    char      pattern[128];   /* glob pattern e.g. "*.so", "*.bin" */
    SpeedTier tier;
} TierRule;

typedef struct {
    FeatureFlags features;

    TierRule  tier_rules[IDKFS_MAX_TIER_RULES];
    int       tier_rule_count;
    SpeedTier default_tier;

    char      sorting[32];    /* "btree", "hashmap", "radix" */
    bool      loaded;         /* was a config file found? */
} IDKFSConfig;

static IDKFSConfig g_config = {0};

static void config_defaults(IDKFSConfig *cfg) {
    cfg->features     = features_default();
    cfg->default_tier = TIER_NORMAL;
    cfg->loaded       = false;
    strncpy(cfg->sorting, "btree", sizeof(cfg->sorting)-1);

    /* sensible defaults: binaries fast, archives slow */
    int n = 0;
    #define ADD_RULE(pat, t) \
        strncpy(cfg->tier_rules[n].pattern, pat, 127); \
        cfg->tier_rules[n].tier = t; n++;

    ADD_RULE("*.so",    TIER_FAST)
    ADD_RULE("*.so.*",  TIER_FAST)
    ADD_RULE("*.bin",   TIER_FAST)
    ADD_RULE("*.o",     TIER_FAST)
    ADD_RULE("*.exe",   TIER_FAST)
    ADD_RULE("*.iso",   TIER_SLOW)
    ADD_RULE("*.tar",   TIER_SLOW)
    ADD_RULE("*.tar.*", TIER_SLOW)
    ADD_RULE("*.zip",   TIER_SLOW)
    ADD_RULE("*.zst",   TIER_SLOW)
    ADD_RULE("*.gz",    TIER_SLOW)
    ADD_RULE("*.bz2",   TIER_SLOW)

    #undef ADD_RULE
    cfg->tier_rule_count = n;
}

/* ============================================================
 * TIER LOOKUP — given a filename, return its tier
 * ============================================================ */

SpeedTier config_get_tier(IDKFSConfig *cfg, const char *filename) {
    for (int i = 0; i < cfg->tier_rule_count; i++) {
        if (fnmatch(cfg->tier_rules[i].pattern, filename, FNM_NOESCAPE) == 0)
            return cfg->tier_rules[i].tier;
    }
    return cfg->default_tier;
}

/* ============================================================
 * JS HELPERS — read fields from a JS object
 * ============================================================ */

static bool js_get_bool(JSContext *ctx, JSValue obj, const char *key, bool fallback) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(v) || JS_IsException(v)) { JS_FreeValue(ctx, v); return fallback; }
    bool result = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return result;
}

static void js_get_str(JSContext *ctx, JSValue obj, const char *key, char *out, size_t maxlen) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(v) || JS_IsException(v)) { JS_FreeValue(ctx, v); return; }
    const char *s = JS_ToCString(ctx, v);
    if (s) { strncpy(out, s, maxlen-1); JS_FreeCString(ctx, s); }
    JS_FreeValue(ctx, v);
}

/* ============================================================
 * PARSE CONFIG — read idkfs.config.js via QuickJS
 * ============================================================ */

static void parse_disable(JSContext *ctx, JSValue root, FeatureFlags *f) {
    JSValue dis = JS_GetPropertyStr(ctx, root, "disable");
    if (JS_IsUndefined(dis) || JS_IsException(dis)) { JS_FreeValue(ctx, dis); return; }

    f->journaling  = !js_get_bool(ctx, dis, "journaling",  !f->journaling);
    f->checksums   = !js_get_bool(ctx, dis, "checksums",   !f->checksums);
    f->timestamps  = !js_get_bool(ctx, dis, "timestamps",  !f->timestamps);
    f->compression = !js_get_bool(ctx, dis, "compression", !f->compression);
    f->encryption  = !js_get_bool(ctx, dis, "encryption",  !f->encryption);
    f->dedup       = !js_get_bool(ctx, dis, "dedup",       !f->dedup);
    f->prefetch    = !js_get_bool(ctx, dis, "prefetch",    !f->prefetch);
    f->lua_hooks   = !js_get_bool(ctx, dis, "lua_hooks",   !f->lua_hooks);
    f->sorting     = !js_get_bool(ctx, dis, "sorting",     !f->sorting);

    JS_FreeValue(ctx, dis);
}

static void parse_speed_tiers(JSContext *ctx, JSValue root, IDKFSConfig *cfg) {
    JSValue st = JS_GetPropertyStr(ctx, root, "speed_tier");
    if (JS_IsUndefined(st) || JS_IsException(st)) { JS_FreeValue(ctx, st); return; }

    const char *tier_names[] = {"fast", "normal", "slow"};
    SpeedTier   tier_vals[]  = {TIER_FAST, TIER_NORMAL, TIER_SLOW};

    cfg->tier_rule_count = 0;

    for (int t = 0; t < 3; t++) {
        JSValue arr = JS_GetPropertyStr(ctx, st, tier_names[t]);
        if (JS_IsUndefined(arr) || !JS_IsArray(ctx, arr)) { JS_FreeValue(ctx, arr); continue; }

        JSValue len_v = JS_GetPropertyStr(ctx, arr, "length");
        int len = 0; JS_ToInt32(ctx, &len, len_v); JS_FreeValue(ctx, len_v);

        for (int i = 0; i < len && cfg->tier_rule_count < IDKFS_MAX_TIER_RULES; i++) {
            JSValue item = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
            const char *s = JS_ToCString(ctx, item);
            if (s) {
                strncpy(cfg->tier_rules[cfg->tier_rule_count].pattern, s, 127);
                cfg->tier_rules[cfg->tier_rule_count].tier = tier_vals[t];
                cfg->tier_rule_count++;
                JS_FreeCString(ctx, s);
            }
            JS_FreeValue(ctx, item);
        }
        JS_FreeValue(ctx, arr);
    }

    JS_FreeValue(ctx, st);
}

static void parse_sorting(JSContext *ctx, JSValue root, IDKFSConfig *cfg) {
    JSValue s = JS_GetPropertyStr(ctx, root, "sorting");
    if (JS_IsUndefined(s) || JS_IsException(s)) { JS_FreeValue(ctx, s); return; }
    if (JS_IsObject(s)) {
        js_get_str(ctx, s, "algorithm", cfg->sorting, sizeof(cfg->sorting));
    }
    JS_FreeValue(ctx, s);
}

int idkfs_config_load(IDKFSConfig *cfg, const char *config_path) {
    config_defaults(cfg);

    FILE *f = fopen(config_path, "r");
    if (!f) {
        printf("idkfs: no config file at '%s', using defaults\n", config_path);
        return 0;
    }
    fclose(f);

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_init_handlers(rt);
    JS_SetModuleLoaderFunc(rt, NULL, js_module_loader, NULL);

    /* read the file */
    size_t buf_len = 0;
    uint8_t *buf = js_load_file(ctx, &buf_len, config_path);
    if (!buf) {
        fprintf(stderr, "idkfs: failed to read config file\n");
        JS_FreeContext(ctx); JS_FreeRuntime(rt);
        return -1;
    }

    JSValue result = JS_Eval(ctx, (const char *)buf, buf_len, config_path,
                             JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT);
    js_free(ctx, buf);

    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, exc);
        fprintf(stderr, "idkfs: JS error in config: %s\n", msg ? msg : "unknown");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, result);
        JS_FreeContext(ctx); JS_FreeRuntime(rt);
        return -1;
    }
    JS_FreeValue(ctx, result);

    /* get the default export — expected: globalThis.default or just an object named 'config' */
    JSValue global = JS_GetGlobalObject(ctx);

    /* try 'export default' style — QuickJS global eval puts it in globalThis */
    JSValue cfg_obj = JS_GetPropertyStr(ctx, global, "default");
    if (JS_IsUndefined(cfg_obj) || JS_IsException(cfg_obj)) {
        JS_FreeValue(ctx, cfg_obj);
        /* fallback: try 'config' variable */
        cfg_obj = JS_GetPropertyStr(ctx, global, "config");
    }

    if (!JS_IsObject(cfg_obj)) {
        fprintf(stderr, "idkfs: config must export an object (use 'var default = {...}' or 'var config = {...}')\n");
        JS_FreeValue(ctx, cfg_obj);
        JS_FreeValue(ctx, global);
        JS_FreeContext(ctx); JS_FreeRuntime(rt);
        return -1;
    }

    parse_disable(ctx, cfg_obj, &cfg->features);
    parse_speed_tiers(ctx, cfg_obj, cfg);
    parse_sorting(ctx, cfg_obj, cfg);

    JS_FreeValue(ctx, cfg_obj);
    JS_FreeValue(ctx, global);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    cfg->loaded = true;
    printf("idkfs: loaded config from '%s'\n", config_path);
    printf("idkfs:   sorting=%s  tier_rules=%d  journaling=%d  checksums=%d  timestamps=%d\n",
           cfg->sorting, cfg->tier_rule_count,
           cfg->features.journaling, cfg->features.checksums, cfg->features.timestamps);

    return 0;
}

/* ============================================================
 * PRINT CONFIG — for debugging
 * ============================================================ */

void idkfs_config_print(IDKFSConfig *cfg) {
    const char *tier_names[] = {"FAST","NORMAL","SLOW"};
    printf("=== idkfs config ===\n");
    printf("  loaded:      %s\n", cfg->loaded ? "yes" : "no (defaults)");
    printf("  sorting:     %s\n", cfg->sorting);
    printf("  default_tier: %s\n", tier_names[cfg->default_tier]);
    printf("  features:\n");
    printf("    journaling=%d  checksums=%d  timestamps=%d\n",
           cfg->features.journaling, cfg->features.checksums, cfg->features.timestamps);
    printf("    compression=%d  encryption=%d  dedup=%d\n",
           cfg->features.compression, cfg->features.encryption, cfg->features.dedup);
    printf("    prefetch=%d  lua_hooks=%d  sorting=%d\n",
           cfg->features.prefetch, cfg->features.lua_hooks, cfg->features.sorting);
    printf("  tier rules (%d):\n", cfg->tier_rule_count);
    for (int i = 0; i < cfg->tier_rule_count; i++)
        printf("    %-20s → %s\n", cfg->tier_rules[i].pattern, tier_names[cfg->tier_rules[i].tier]);
}
