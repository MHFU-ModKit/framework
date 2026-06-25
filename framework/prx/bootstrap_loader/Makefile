# Split-bootstrap loader PRX (mhfu_boot.prx). Tiny, float-free; loads the big
# mhfu_framework.prx deferred (psplink pattern). Uses build_prx.mak (-nostartfiles,
# module_start-direct). Build via the pspdev Docker image.
TARGET = mhfu_boot
OBJS   = main.o

CFLAGS   = -Os -G0 -Wall
CXXFLAGS = $(CFLAGS)
ASFLAGS  = $(CFLAGS)

LIBS = -lpspsdk -lpspuser

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak
