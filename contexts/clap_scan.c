#include "clap_scan.h"
#include <clap/clap.h>
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>

// once the plugin is loaded, leave without running its static destructors:
// nothing needs them and a plugin that crashes on unload would fail the scan
_Noreturn static void scan_exit(int status) {
    fflush(stderr);
    _exit(status);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <file.clap>\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];

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

    const clap_plugin_factory_t *factory =
        (const clap_plugin_factory_t *)entry->get_factory(
            CLAP_PLUGIN_FACTORY_ID);
    uint32_t count = factory ? factory->get_plugin_count(factory) : 0;
    for (uint32_t i = 0; i < count; i++) {
        const clap_plugin_descriptor_t *desc =
            factory->get_plugin_descriptor(factory, i);
        if (!desc || !desc->id || !desc->name)
            continue;
        fprintf(out, "%u%c%s%c%s%c", i, '\0', desc->id, '\0', desc->name,
                '\0');
    }
    fputc('\0', out);
    int status = fflush(out) == 0 ? 0 : 1;

    entry->deinit();
    scan_exit(status);
}
