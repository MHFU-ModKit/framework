# Build hello_world.prx — depends on the framework headers and the
# framework PRX's exported symbols.

DOCKER_IMG  = pspdev/pspdev:latest
# Mount the MHFU repo root so this mod can reach ../../include (the
# framework headers). Levels: this Makefile lives 4 deep under root.
REPO_ROOT   = $(shell cd ../../../.. && pwd)
MOD_REL     = framework/prx/mods/hello_world_prx

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
