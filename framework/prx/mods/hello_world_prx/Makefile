# Build hello_world.prx — depends on the framework headers and the
# framework PRX's exported symbols.

DOCKER_IMG  = pspdev/pspdev:latest
PROJECT_DIR = $(shell pwd)

.PHONY: all clean host-make shell

all:
	docker run --rm -v "$(PROJECT_DIR)/../../..":/work -w /work/framework/prx/mods/hello_world_prx $(DOCKER_IMG) \
	    make -f Makefile.psp

clean:
	rm -f *.o *.elf *.prx src/*.o

host-make:
	make -f Makefile.psp

shell:
	docker run --rm -it -v "$(PROJECT_DIR)/../../..":/work -w /work/framework/prx/mods/hello_world_prx $(DOCKER_IMG) bash
