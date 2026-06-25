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

.PHONY: all realhw clean host-make shell

all:
	docker run --rm -v "$(PROJECT_DIR)":/work -w /work $(DOCKER_IMG) \
	    bash -c "psp-pacman -S --noconfirm pspsdk-bin >/dev/null 2>&1 || true; make -f Makefile.psp"
	@# Real-PSP fix part 2: clear EF_MIPS_ABI_EABI32 from e_flags (psp-prxgen copies
	@# it verbatim; old firmware/PRO CFW reject it -> 0x800200D9). Host-side (the
	@# pspdev image has no python). See tools/patch_prx_eflags.py + Makefile.psp.
	python3 tools/patch_prx_eflags.py mhfu_framework.prx

# Real-PSP-hardware build (allocates inject scratch from the ARK-4-grown user
# partition instead of the PPSSPP raw extra-RAM window). cleans first so every TU
# picks up the define, then renames the output to mhfu_framework_realhw.prx.
realhw:
	docker run --rm -v "$(PROJECT_DIR)":/work -w /work $(DOCKER_IMG) \
	    bash -c "psp-pacman -S --noconfirm pspsdk-bin >/dev/null 2>&1 || true; \
	             make -f Makefile.psp clean >/dev/null 2>&1 || true; \
	             make -f Makefile.psp EXTRA_DEFINES=-DMHFU_REALHW && \
	             cp mhfu_framework.prx mhfu_framework_realhw.prx"

clean:
	rm -f *.o *.elf *.prx *.SFO *.PBP src/*.o src/core/*.o mods/*/mod.o mods/*/*/mod.o

host-make:
	make -f Makefile.psp

shell:
	docker run --rm -it -v "$(PROJECT_DIR)":/work -w /work $(DOCKER_IMG) bash
