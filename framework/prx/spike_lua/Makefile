# Phase-0 Lua spike — standalone runnable EBOOT.
TARGET = lua_spike
OBJS = main.o

INCDIR   =
CFLAGS   = -O2 -G0 -Wall
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS  = $(CFLAGS)

LIBDIR =
LDFLAGS =
# link order matters: lua needs libm; psp libs after
LIBS = -llua -lm -lstdc++

EXTRA_TARGETS   = EBOOT.PBP
PSP_EBOOT_TITLE = MHFU Lua Spike

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
