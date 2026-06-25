# Tiny diagnostic PRX. Uses build_prx.mak (like psplink) — that links with
# -nostartfiles (NO crt0), so we define module_start directly and the plugin does
# NOT auto-create a main thread / grab a default heap at load. The crt0/main()
# pattern (build.mak + prxspecs) is what fails to load on MHFU's bare 24 MB
# partition (0x800200D9). Build via the pspdev Docker image.
TARGET = mhfu_memprobe
OBJS   = main.o

CFLAGS   = -Os -G0 -Wall
CXXFLAGS = $(CFLAGS)
ASFLAGS  = $(CFLAGS)

# prxexports.o (first) = the MANDATORY syslib export (module_start/module_info via
# .lib.ent). build_prx.mak does NOT auto-link it (build.mak does), so without it the
# PRX has an empty export table and real PSP firmware rejects it (0x800200D9) even
# though PPSSPP loads it. THE real-PSP load fix.
LIBS = $(PSPSDK)/lib/prxexports.o -lpspsdk -lpspuser

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak

# Real-PSP fix part 1: discard the modern-toolchain .MIPS.abiflags/.reginfo via the
# shared linker script one dir up. (Part 2 — clearing EF_MIPS_ABI_EABI32 from
# e_flags — is applied host-side with tools/patch_prx_eflags.py after the build,
# since the pspdev image has no python.) Rebuild LDFLAGS so our -T is the only one.
LIBDIR_FLAGS := $(addprefix -L,$(LIBDIR))
override LDFLAGS := $(LIBDIR_FLAGS) -Wl,-q,-T$(CURDIR)/../linkfile_nohw.lds -nostartfiles -Wl,-zmax-page-size=128
