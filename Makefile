#compiler name
CC=gcc
#name of the binary filename
FILE=build/smp_sampler

#include dirs
#INCDIR = -I$(PORTSDIR)/include
INCDIR = -I/usr/include

#additional libs
LIBS = -lm -ljack -lsndfile -ljson-c -llilv-0

#jalv source code
JALV_C = util_funcs/jalv/symap.c util_funcs/jalv/worker.c util_funcs/jalv/zix/allocator.c util_funcs/jalv/zix/allocator.h util_funcs/jalv/zix/attributes.h util_funcs/jalv/zix/ring.c

#clap additional extension code
CLAP_EXT_C = contexts/clap_ext/clap_ext_preset_factory.c
#util functions
UTIL_FUNCS = util_funcs/wav_funcs.c util_funcs/math_funcs.c util_funcs/string_funcs.c util_funcs/json_funcs.c util_funcs/ring_buffer.c util_funcs/log_funcs.c util_funcs/osc_wavelookup.c util_funcs/uniform_buffer.c util_funcs/path_funcs.c util_funcs/array_utils.c
#additional sources
SRC = $(UTIL_FUNCS) contexts/sampler.c contexts/plugins.c contexts/clap_plugins.c contexts/context_control.c jack_funcs/jack_funcs.c app_data.c app_intrf.c contexts/params.c contexts/synth.c $(JALV_C) $(CLAP_EXT_C)

#Remote dir for the source code
PI_DIR = ~/Audio/Source/smp_groovebox/

MAIN_SRC = smp_groovebox_ui_cli.c
MAIN_CLI_SRC = smp_groovebox_ui_cli.c

create_smp_sampler: make_dir
	$(CC) -g -x c -o $(FILE) $(MAIN_SRC) $(SRC) $(INCDIR) $(LIBDIRS) $(LIBS)
build_sanitize: make_dir
	$(CC) -g -fsanitize=thread -x c -o $(FILE) $(MAIN_CLI_SRC) $(SRC) $(INCDIR) $(LIBDIRS) $(LIBS)
run:
	(cd build && ./smp_sampler)
run_valgrind:
	(cd build && valgrind --leak-check=full --show-leak-kinds=all --log-file=val_log ./smp_sampler)
clean_build:
	(cd build && rm -r *)
make_dir:
	mkdir -p build/

.PHONY: rsync_src
pi_build: rsync_src
	ssh pi_daw make -C $(PI_DIR)
pi_build_sanitize: rsync_src
	ssh pi_daw make build_sanitize -C $(PI_DIR)
pi_goto_build:
	ssh pi_daw -t "cd $(PI_DIR)/build/ ; bash --login"
pi_run:
	ssh pi_daw -t "cd $(PI_DIR)/build/ && ./smp_sampler"
pi_run_valgrind:
	ssh pi_daw -t "cd $(PI_DIR)/build/ && valgrind --leak-check=full --log-file=val_log ./smp_sampler"
rsync_src:
	rsync -va --exclude 'build' * pi_daw:$(PI_DIR)
