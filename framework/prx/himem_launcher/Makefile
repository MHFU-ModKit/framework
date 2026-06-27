# mhfu_himem_launcher — homebrew EBOOT. Sets the separate p8 partition via the CFW,
# then loadexecs the MHFU ISO directly (so MHFU FRESH-BOOTS with p8; the launcher
# tears down, not MHFU — MHFU never experiences a teardown-with-p8, which is what froze).
TARGET = mhfu_himem_launcher
OBJS   = main.o

CFLAGS   = -Os -G0 -Wall
CXXFLAGS = $(CFLAGS)
ASFLAGS  = $(CFLAGS)

# user-mode CFW stubs — sctrlSEMountUmdFromFile/LoadExecVSHDisc work from user mode
# (they syscall into the kernel CFW). User EBOOT loads fine; kernel EBOOT didn't.
LIBS = -lpspsystemctrl_user

EXTRA_TARGETS   = EBOOT.PBP
PSP_EBOOT_TITLE = MHFU HiMem Launcher

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
