#compiler name
CC=gcc
#name of the binary filename
BUILD_DIR=build
BUILD_FILE=smp_grvbx
FILE=$(BUILD_DIR)/$(BUILD_FILE)
#the clap scanner, run by the app from the directory of its binary

SCAN_FILE=build/smp_clap_scan

# user interfaces, choose one
UI_DEFAULT = smp_groovebox_ui_cli.c
UI_CLI = smp_groovebox_ui_cli.c

#include dirs
#INCDIR = -I$(PORTSDIR)/include
INCDIR = -I/usr/include

#additional libs
LIBS = -lm -ljack -lsndfile -ljson-c -llilv-0

#jalv source code
JALV_C = util_funcs/jalv/symap.c util_funcs/jalv/worker.c util_funcs/jalv/zix/allocator.c util_funcs/jalv/zix/allocator.h util_funcs/jalv/zix/attributes.h util_funcs/jalv/zix/ring.c

#clap additional extension code
CLAP_EXT_C = contexts/clap_ext/clap_ext_preset_load.c
#the clap scanner's sources - plugin code that runs out of the app's process
SCAN_C = contexts/clap_scan.c contexts/clap_ext/clap_ext_preset_factory.c
#util functions
UTIL_FUNCS = util_funcs/wav_funcs.c util_funcs/math_funcs.c util_funcs/string_funcs.c util_funcs/ring_buffer.c util_funcs/log_funcs.c util_funcs/osc_wavelookup.c util_funcs/uniform_buffer.c util_funcs/midi_buf.c util_funcs/path_funcs.c util_funcs/hash_table.c util_funcs/intern_table.c util_funcs/tree_index.c
#the audio engine, knows no backend
ENGINE = engine/graph.c
#additional sources
SRC = $(UTIL_FUNCS) $(ENGINE) contexts/sampler.c contexts/plugins.c contexts/clap_plugins.c contexts/context_control.c jack_funcs/jack_funcs.c app_data.c app_intrf.c ui_layer.c contexts/params.c contexts/synth.c $(JALV_C) $(CLAP_EXT_C)

#Remote dir for the source code
PI_DIR = ~/Audio/Source/smp_groovebox/


create_smp_sampler: make_dir clap_scanner
	$(CC) -Wall -Wextra -Wshadow -g -x c -o $(FILE) $(UI_DEFAULT) $(SRC) $(INCDIR) $(LIBDIRS) $(LIBS)
build_release: make_dir clap_scanner
	$(CC) -Wall -Wextra -Wshadow -O2 -g -x c -o $(FILE) $(UI_DEFAULT) $(SRC) $(INCDIR) $(LIBDIRS) $(LIBS)
#thread sanitizer can not be combined with address sanitizer, so two targets
build_sanitize: make_dir clap_scanner
	$(CC) -Wall -Wextra -Wshadow -O1 -g -fno-omit-frame-pointer -fsanitize=thread,undefined -x c -o $(FILE) $(UI_CLI) $(SRC) $(INCDIR) $(LIBDIRS) $(LIBS)
build_asan: make_dir clap_scanner
	$(CC) -Wall -Wextra -Wshadow -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -x c -o $(FILE) $(UI_CLI) $(SRC) $(INCDIR) $(LIBDIRS) $(LIBS)
clap_scanner: make_dir
	$(CC) -Wall -Wextra -Wshadow -g -o $(SCAN_FILE) $(SCAN_C) $(INCDIR)
run:
	(cd $(BUILD_DIR) && ./$(BUILD_FILE))
run_valgrind:
	(cd $(BUILD_DIR) && valgrind --leak-check=full --show-leak-kinds=all --log-file=val_log ./$(BUILD_FILE))
clean_build:
	(cd $(BUILD_DIR) && rm -r *)
make_dir:
	mkdir -p $(BUILD_DIR)/

.PHONY: rsync_src
pi_build: rsync_src
	ssh pi_daw make -C $(PI_DIR)
pi_build_release: rsync_src
	ssh pi_daw make build_release -C $(PI_DIR)
pi_build_sanitize: rsync_src
	ssh pi_daw make build_sanitize -C $(PI_DIR)
pi_build_asan: rsync_src
	ssh pi_daw make build_asan -C $(PI_DIR)
pi_goto_build:
	ssh pi_daw -t "cd $(PI_DIR)/$(BUILD_DIR)/ ; bash --login"
pi_run:
	ssh pi_daw -t "cd $(PI_DIR)/$(BUILD_DIR)/ && ./$(BUILD_FILE)"
pi_run_valgrind:
	ssh pi_daw -t "cd $(PI_DIR)/$(BUILD_DIR)/ && valgrind --leak-check=full --log-file=val_log ./$(BUILD_FILE)"
rsync_src:
	rsync -va --exclude '$(BUILD_DIR)' * pi_daw:$(PI_DIR)
