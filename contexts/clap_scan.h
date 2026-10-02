#pragma once
// The clap scanner (clap_scan.c) runs plugin code for the app in a process of
// its own, so nothing a plugin does there can leak into or crash the app.
// Run from the directory of the app's binary. Everything is written to stdout
// as '\0' terminated fields, ending with one empty field where a record would
// start - output without that end mark is incomplete.
//
// `CLAP_SCAN_EXE <file.clap>` lists the plugins in the file, per plugin: its
// factory index, id and name.
//
// `CLAP_SCAN_EXE CLAP_SCAN_PRESETS_ARG <file.clap> <plugin id>` lists the
// presets of the file's preset-discovery factory for that plugin, in discovery
// order. Per preset: the number of category path segments, the segments, its
// name, its location (empty for a preset inside the plugin) and its load key
// (empty for a whole file preset).

// matches SCAN_FILE in the Makefile
#define CLAP_SCAN_EXE "smp_clap_scan"
#define CLAP_SCAN_PRESETS_ARG "--presets"
