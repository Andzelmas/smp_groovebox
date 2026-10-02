#include "clap_scan.h"
#include "clap_ext/clap_ext_preset_factory.h"
#include <clap/clap.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// once the plugin is loaded, leave without running its static destructors:
// nothing needs them and a plugin that crashes on unload would fail the scan
_Noreturn static void scan_exit(int status) {
    fflush(stderr);
    _exit(status);
}

static void scan_field(FILE *out, const char *field) {
    fputs(field ? field : "", out);
    fputc('\0', out);
}

static void scan_plugins(FILE *out, const clap_plugin_entry_t *entry) {
    const clap_plugin_factory_t *factory =
        (const clap_plugin_factory_t *)entry->get_factory(
            CLAP_PLUGIN_FACTORY_ID);
    uint32_t count = factory ? factory->get_plugin_count(factory) : 0;
    for (uint32_t i = 0; i < count; i++) {
        const clap_plugin_descriptor_t *desc =
            factory->get_plugin_descriptor(factory, i);
        if (!desc || !desc->id || !desc->name)
            continue;
        fprintf(out, "%u%c", i, '\0');
        scan_field(out, desc->id);
        scan_field(out, desc->name);
    }
}

static void scan_preset_emit(void *user_data, const char *const *path,
                             size_t depth, const char *name,
                             const char *location, const char *load_key) {
    FILE *out = user_data;
    fprintf(out, "%zu%c", depth, '\0');
    for (size_t i = 0; i < depth; i++)
        scan_field(out, path[i]);
    scan_field(out, name);
    scan_field(out, location);
    scan_field(out, load_key);
}

int main(int argc, char **argv) {
    bool presets = argc == 4 && strcmp(argv[1], CLAP_SCAN_PRESETS_ARG) == 0;
    if (argc != 2 && !presets) {
        fprintf(stderr,
                "usage: %s <file.clap>\n       %s %s <file.clap> <plugin id>\n",
                argv[0], argv[0], CLAP_SCAN_PRESETS_ARG);
        return 2;
    }
    const char *path = presets ? argv[2] : argv[1];

    // the listing keeps stdout to itself, whatever the plugin prints goes to
    // stderr
    int out_fd = dup(STDOUT_FILENO);
    FILE *out = out_fd >= 0 ? fdopen(out_fd, "w") : NULL;
    if (!out || dup2(STDERR_FILENO, STDOUT_FILENO) < 0)
        return 1;

    void *handle = dlopen(path, RTLD_LOCAL | RTLD_LAZY);
    if (!handle) {
        fprintf(stderr, "%s\n", dlerror());
        return 1;
    }
    const clap_plugin_entry_t *entry =
        (const clap_plugin_entry_t *)dlsym(handle, "clap_entry");
    if (!entry || !clap_version_is_compatible(entry->clap_version)) {
        fprintf(stderr, "%s: no compatible clap_entry\n", path);
        scan_exit(1);
    }
    if (!entry->init(path)) {
        fprintf(stderr, "%s: init failed\n", path);
        scan_exit(1);
    }

    // a plugin without a preset-discovery factory simply has no presets
    if (presets)
        clap_ext_preset_index(entry, argv[3], CLAP_SCAN_EXE, scan_preset_emit,
                              out);
    else
        scan_plugins(out, entry);
    fputc('\0', out);
    int status = fflush(out) == 0 ? 0 : 1;

    entry->deinit();
    scan_exit(status);
}
