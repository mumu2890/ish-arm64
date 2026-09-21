/*
 * WASM bridge for iOS — loads and executes WASM modules via WAMR.
 * AMFI-safe: WAMR uses pure interpretation (no JIT, no mmap(PROT_EXEC)).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "wasm_export.h"

static bool g_wasm_init = false;
static char g_debug_buf[2048] = {0};

const char *wasm_bridge_get_debug(void) { return g_debug_buf; }

#define DBG(...) do { snprintf(g_debug_buf + strlen(g_debug_buf), sizeof(g_debug_buf) - strlen(g_debug_buf), __VA_ARGS__); } while(0)

void wasm_bridge_init(void)
{
    if (g_wasm_init) return;
    wasm_runtime_init();
    wasm_runtime_set_default_running_mode(Mode_Interp);
    g_wasm_init = true;
    fprintf(stderr, "[wasm] WAMR runtime initialized (interp mode)\n");
}

void *wasm_bridge_load(const char *path)
{
    if (!g_wasm_init) wasm_bridge_init();

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[wasm] cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *buf = malloc(size);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);

    char err[256] = {0};
    wasm_module_t module = wasm_runtime_load(buf, (uint32_t)size, err, sizeof(err));
    // Do NOT free(buf): WAMR's is_load_from_file_buf=true path
    // stores string pointers inside buf. Buffer must stay alive.
    if (!module) free(buf);

    if (!module) {
        fprintf(stderr, "[wasm] load failed: %s\n", err);
        return NULL;
    }

    int exportCount = wasm_runtime_get_export_count(module);
    fprintf(stderr, "[wasm] module loaded: %s (%ld bytes, %d exports)\n", path, size, exportCount);

    // Debug: print all exports to buffer
    for (int i = 0; i < exportCount; i++) {
        wasm_export_t et;
        wasm_runtime_get_export_type(module, i, &et);
        DBG("export %d: name=%s kind=%d\n", i, et.name ? et.name : "(null)", (int)et.kind);
    }
    DBG("lookup main: %s\n", wasm_runtime_lookup_function((wasm_module_inst_t)module, "main") ? "found" : "NULL");

    return (void *)module;
}

void *wasm_bridge_instantiate(void *module, uint32_t stack_size, uint32_t heap_size)
{
    if (!module) return NULL;

    char err[256] = {0};
    wasm_module_inst_t inst = wasm_runtime_instantiate(
        (wasm_module_t)module, stack_size, heap_size, err, sizeof(err));

    if (!inst) {
        fprintf(stderr, "[wasm] instantiate failed: %s\n", err);
        return NULL;
    }

    fprintf(stderr, "[wasm] instance created\n");
    return (void *)inst;
}

void *wasm_bridge_lookup(void *instance, const char *name)
{
    if (!instance) return NULL;

    wasm_function_inst_t func = wasm_runtime_lookup_function(
        (wasm_module_inst_t)instance, name);

    if (!func) {
        fprintf(stderr, "[wasm] lookup failed: %s\n", name);
        return NULL;
    }

    fprintf(stderr, "[wasm] found: %s\n", name);
    return (void *)func;
}

bool wasm_bridge_call(void *instance, void *func, uint32_t *result)
{
    if (!instance || !func) return false;

    wasm_exec_env_t exec_env = wasm_runtime_create_exec_env(
        (wasm_module_inst_t)instance, 8192);
    if (!exec_env) return false;

    uint32_t argv[1] = {0};
    bool ok = wasm_runtime_call_wasm(exec_env, (wasm_function_inst_t)func, 0, argv);

    if (result && ok) {
        *result = argv[0];
    }

    fprintf(stderr, "[wasm] call ok=%d result=%u\n", ok, argv[0]);
    wasm_runtime_destroy_exec_env(exec_env);
    return ok;
}

void wasm_bridge_unload(void *module)
{
    if (!module) return;
    wasm_runtime_unload((wasm_module_t)module);
    fprintf(stderr, "[wasm] module unloaded\n");
}

void wasm_bridge_destroy(void)
{
    if (!g_wasm_init) return;
    wasm_runtime_destroy();
    g_wasm_init = false;
    fprintf(stderr, "[wasm] WAMR runtime destroyed\n");
}

int wasm_bridge_export_count(void *module)
{
    if (!module) return -1;
    return wasm_runtime_get_export_count((wasm_module_t)module);
}
