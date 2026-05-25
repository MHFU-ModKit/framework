# Builds mhfu_framework.prx using pspdev/pspdev Docker image.
#
# Usage:
#   make            -- builds mhfu_framework.prx via Docker
#   make clean      -- removes build artifacts
#   make host-make  -- runs the underlying Makefile inside the container
#
# The actual PSP build is invoked inside the Docker container; this
# top-level Makefile is just a wrapper.

DOCKER_IMG = pspdev/pspdev:latest
PROJECT_DIR = $(shell pwd)

.PHONY: all clean host-make shell

all:
	docker run --rm -v "$(PROJECT_DIR)":/work -w /work $(DOCKER_IMG) \
	    bash -c "psp-pacman -S --noconfirm pspsdk-bin >/dev/null 2>&1 || true; make -f Makefile.psp"

clean:
	rm -f *.o *.elf *.prx *.SFO *.PBP src/*.o

host-make:
	make -f Makefile.psp

shell:
	docker run --rm -it -v "$(PROJECT_DIR)":/work -w /work $(DOCKER_IMG) bash
