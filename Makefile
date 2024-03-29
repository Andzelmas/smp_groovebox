#compiler name
CC=gcc
#name of the binary filename
FILE=build/smp_sampler

#include dirs
INCDIR = -I$(PORTSDIR)/include

#additional libs
LIBS = -lm -ljack -lsndfile -ljson-c -llilv-0 -lncurses

#jalv source code
JALV_C = util_funcs/jalv/symap.c util_funcs/jalv/worker.c util_funcs/jalv/zix/allocator.c util_funcs/jalv/zix/allocator.h util_funcs/jalv/zix/attributes.h util_funcs/jalv/zix/ring.c

#additional sources
SRC = util_funcs/wav_funcs.c contexts/sampler.c contexts/plugins.c contexts/clap_plugins.c jack_funcs/jack_funcs.c util_funcs/math_funcs.c app_data.c app_intrf.c util_funcs/string_funcs.c util_funcs/json_funcs.c util_funcs/ring_buffer.c util_funcs/log_funcs.c contexts/params.c util_funcs/osc_wavelookup.c contexts/synth.c $(JALV_C)

#Remote dir for the source code
PI_DIR = ~/Audio/Source/smp_groovebox/

MAIN_SRC = smp_sampler_ncurses.c

create_smp_sampler:
	$(CC) -g -x c -o $(FILE) $(MAIN_SRC) $(SRC) $(INCDIR) $(LIBDIRS) $(LIBS)
build_sanitize:
	$(CC) -g -fsanitize=thread -x c -o $(FILE) $(MAIN_SRC) $(SRC) $(INCDIR) $(LIBDIRS) $(LIBS)
run:
	(cd build && ./smp_sampler)
run_valgrind:
	(cd build && valgrind --leak-check=full --log-file=val_log ./smp_sampler)
clean_build:
	(cd build && rm -r *)

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
