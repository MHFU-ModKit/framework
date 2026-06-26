# Split-bootstrap loader PRX (mhfu_boot.prx). Tiny, float-free KERNEL module; loads
# the big USER-mode mhfu_framework.prx deferred, in-game (psplink pattern). Uses
# build_prx.mak (-nostartfiles, module_start-direct). Build via the pspdev Docker image.
TARGET = mhfu_boot
OBJS   = main.o

CFLAGS   = -Os -G0 -Wall
CXXFLAGS = $(CFLAGS)
ASFLAGS  = $(CFLAGS)

# prxexports.o (first) = the MANDATORY syslib export (module_start/module_info via
# .lib.ent). build_prx.mak does NOT auto-link it; without it real PSP firmware rejects
# the PRX (0x800200D9) even though PPSSPP loads it. THE real-PSP load fix.
#
# -lpspkernel (NOT -lpspuser) — THE deferred-load fix. sceKernelLoadModule resolves
# from libpspuser to the ModuleMgrForUser SYSCALL stub, which is permission-gated and
# rejects a KERNEL-thread caller with 0x80020149 (ILLEGAL_PERM_CALL) — pspSdkSetK1(0)
# does NOT bypass it (the gate is the syscall library itself, not the k1 pointer check).
# libpspkernel resolves both sceKernelLoadModule/sceKernelStartModule to ModuleMgrFor-
# KERNEL = a direct kernel call with no permission gate. This is exactly how psplink
# (USE_KERNEL_LIBS=1) loads its user PRX from a kernel module. libpspkernel also covers
# our sceIo*/thread/MaxFree calls (kernel variants); -lpspsdk supplies pspSdkSetK1.
# Order matters: -lpspkernel FIRST so the module-load symbols bind there, not in libpspsdk.
LIBS = $(PSPSDK)/lib/prxexports.o -lpspkernel -lpspsdk

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak

# Drop the modern-toolchain .MIPS.abiflags/.reginfo via the shared linker script one
# dir up (matches the memprobe build). e_flags EABI bit is cleared host-side with
# tools/patch_prx_eflags.py after the build.
LIBDIR_FLAGS := $(addprefix -L,$(LIBDIR))
override LDFLAGS := $(LIBDIR_FLAGS) -Wl,-q,-T$(CURDIR)/../linkfile_nohw.lds -nostartfiles -Wl,-zmax-page-size=128
