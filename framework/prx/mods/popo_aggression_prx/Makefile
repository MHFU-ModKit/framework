# Build popo_growth.prx — first real runtime mod for MHFU.
# Mirrors hello_world_prx wrapping for the pspdev Docker build.

DOCKER_IMG  = pspdev/pspdev:latest
REPO_ROOT   = $(shell cd ../../../.. && pwd)
MOD_REL     = framework/prx/mods/popo_aggression_prx

.PHONY: all clean host-make shell

all:
	docker run --rm -v "$(REPO_ROOT)":/work -w /work/$(MOD_REL) $(DOCKER_IMG) \
	    make -f Makefile.psp

clean:
	rm -f *.o *.elf *.prx src/*.o

host-make:
	make -f Makefile.psp

shell:
	docker run --rm -it -v "$(REPO_ROOT)":/work -w /work/$(MOD_REL) $(DOCKER_IMG) bash
