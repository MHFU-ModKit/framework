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

LIBS = -lpspsdk -lpspuser

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak
