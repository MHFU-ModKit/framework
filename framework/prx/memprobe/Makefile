# Tiny diagnostic PRX — logs free partition memory to ms0. Build via the pspdev
# Docker image (same as the framework). Produces mhfu_memprobe.prx (a few KB).
TARGET = mhfu_memprobe
OBJS   = main.o

CFLAGS   = -Os -G0 -Wall -ffunction-sections -fdata-sections
CXXFLAGS = $(CFLAGS)
ASFLAGS  = $(CFLAGS)
LDFLAGS  = -Wl,--gc-sections -Wl,-u,module_info

BUILD_PRX = 1

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
