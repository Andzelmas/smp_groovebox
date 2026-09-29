#pragma once
// The clap scanner (clap_scan.c) lists the plugins in one .clap file from a
// process of its own, so listing never loads a plugin into the app.
// Run as `CLAP_SCAN_EXE <file.clap>` from the directory of the app's binary.
// On stdout, per plugin: its factory index, id and name, each '\0' terminated.
// Then one empty field once the whole file is listed - output without that end
// mark is incomplete.

// matches SCAN_FILE in the Makefile
#define CLAP_SCAN_EXE "smp_clap_scan"
