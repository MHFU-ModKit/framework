# mhfu_memgrow.prx — VSH plugin (seplugins/vsh.txt). Tiny KERNEL module; calls
# sctrlHENSetMemory in the XMB so MHFU's next launch gets a modestly bigger user
# partition (extra RAM, Slim+). build_prx.mak (-nostartfiles, module_start-direct).
TARGET = mhfu_memgrow
OBJS   = main.o

CFLAGS   = -Os -G0 -Wall
CXXFLAGS = $(CFLAGS)
ASFLAGS  = $(CFLAGS)

# prxexports.o (first) = the MANDATORY syslib export (the real-PSP load fix; without
# it firmware rejects the PRX 0x800200D9). -lpspsystemctrl_kernel provides
# sctrlHENSetMemory; -lpspkernel = kernel sysmem/thread/io stubs (ModuleMgrForKernel
# etc.). Order: systemctrl + kernel BEFORE sdk so symbols bind to the kernel variants.
LIBS = $(PSPSDK)/lib/prxexports.o -lpspsystemctrl_kernel -lpspkernel -lpspsdk

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak

# Drop .MIPS.abiflags/.reginfo via the shared linker script one dir up (matches the
# bootstrap/memprobe build). e_flags EABI bit cleared host-side post-build.
LIBDIR_FLAGS := $(addprefix -L,$(LIBDIR))
override LDFLAGS := $(LIBDIR_FLAGS) -Wl,-q,-T$(CURDIR)/../linkfile_nohw.lds -nostartfiles -Wl,-zmax-page-size=128
