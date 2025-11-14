# ===================================================================
# BoxOS Makefile - Cross-platform OS build system
# ===================================================================
# Builds: disk image, floppy image, ISO, VDI, ELF with debug symbols
# Supports: Linux, macOS, Windows (Cygwin/MSYS2)
# ===================================================================

UNAME_S := $(shell uname -s)

# OS-specific toolchain selection
ifeq ($(UNAME_S),Darwin)  # macOS
    CC       = x86_64-elf-gcc
    LD       = x86_64-elf-ld
    # Alternative: use Homebrew gcc if cross-compiler not available
    # CC       = gcc-13
    # LD       = ld
else ifeq ($(UNAME_S),Linux)  # Linux
    CC       = gcc
    LD       = ld
else  # Windows (Cygwin/MSYS2) or other
    CC       = gcc
    LD       = ld
endif

# ==== TOOLS ====
ASM      = nasm
#CC       = gcc
#LD       = ld
# CC       = x86_64-elf-gcc
# LD       = x86_64-elf-ld
OBJCOPY  = objcopy
QEMU     = qemu-system-x86_64

# ==== FLAGS ====
# === BUILD CONFIGURATION ===
# Bootloader layout:
#   Sector 1     : Stage1 (512 bytes, MBR)
#   Sectors 2-10 : Stage2 (9 sectors = 4608 bytes)
#   Sectors 11+  : Kernel (320 sectors = 163840 bytes = 160KB)
STAGE2_SECTORS      = 9
KERNEL_SECTORS      = 320
KERNEL_MAX_BYTES    = 163840    # 320 * 512
KERNEL_START_SECTOR = 10

ASMFLAGS       =  -g -f bin
ASMFLAGS_ELF   = -g -f elf64
CFLAGS         = -g -m64 -ffreestanding -nostdlib -Wall -Wextra
INCLUDE_DIRS   := $(shell find src -type d)
CFLAGS         += $(addprefix -I,$(INCLUDE_DIRS))
LDFLAGS        = -g -T $(ENTRYDIR)/linker.ld -nostdlib -z max-page-size=0x1000 --oformat=binary

# ==== DIRECTORIES ====
SRCDIR      = src
BUILDDIR    = build
BOOTDIR     = $(SRCDIR)/boot
KERNELDIR   = $(SRCDIR)/kernel
ENTRYDIR    = $(KERNELDIR)/entry

# ==== SOURCES ====
STAGE1_SRC        = $(BOOTDIR)/stage1/stage1.asm
STAGE2_SRC        = $(BOOTDIR)/stage2/stage2.asm
KERNEL_ENTRY_SRC  = $(ENTRYDIR)/kernel_entry.asm

# ==== DISCOVER FILES ====
C_SRCS      := $(shell find $(SRCDIR) -name '*.c')
ASM_SRCS    := $(shell find $(SRCDIR) -name '*.asm')

# ==== EXCLUSIONS ====
BOOT_ASM_SRCS     := $(STAGE1_SRC) $(STAGE2_SRC)
EXCLUDED_ASM_SRCS := $(BOOT_ASM_SRCS) $(KERNEL_ENTRY_SRC)
ASM_SRCS          := $(filter-out $(EXCLUDED_ASM_SRCS),$(ASM_SRCS))

# ==== OBJECT FILES ====
C_OBJS       := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(C_SRCS))
ASM_OBJS     := $(patsubst $(SRCDIR)/%.asm,$(BUILDDIR)/%.o,$(ASM_SRCS))
KERNEL_ENTRY_OBJ := $(patsubst $(SRCDIR)/%.asm,$(BUILDDIR)/%.o,$(KERNEL_ENTRY_SRC))

# ==== FINAL BINARIES ====
KERNEL_BIN   = $(BUILDDIR)/kernel.bin
KERNEL_ELF   = $(BUILDDIR)/kernel.elf
STAGE1_BIN   = $(BUILDDIR)/stage1.bin
STAGE2_BIN   = $(BUILDDIR)/stage2.bin
IMAGE        = $(BUILDDIR)/boxos.img
FLOPPY_IMG   = $(BUILDDIR)/boxos_floppy.img
ISO          = $(BUILDDIR)/boxos.iso
ISO_DIR      = $(BUILDDIR)/isofiles
VBOX_VDI     = $(BUILDDIR)/boxos.vdi

.PHONY: all clean run debug info check-deps install-deps

# ==== MAIN TARGET ====
all: check-deps $(IMAGE) $(KERNEL_ELF) $(FLOPPY_IMG) $(ISO) $(VBOX_VDI)

# ==== DEP CHECK ====
check-deps:
	@echo "Checking dependencies..."
	@command -v $(ASM) >/dev/null || (echo "ERROR: nasm not found" && exit 1)
	@command -v $(CC) >/dev/null || (echo "ERROR: gcc not found" && exit 1)
	@command -v $(LD) >/dev/null || (echo "ERROR: ld not found" && exit 1)
	@command -v $(QEMU) >/dev/null || (echo "ERROR: qemu not found" && exit 1)
	@command -v xorriso >/dev/null || (echo "ERROR: xorriso not found" && exit 1)
	@command -v VBoxManage >/dev/null || (echo "WARNING: VBoxManage not found" && sleep 2)
	@echo "All dependencies OK."

# ==== BUILD RULES ====
$(BUILDDIR):
	@mkdir -p $(BUILDDIR)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	@echo "Compiling $<..."
	@mkdir -p $(@D)
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.asm | $(BUILDDIR)
	@echo "Assembling $<..."
	@mkdir -p $(@D)
	@$(ASM) $(ASMFLAGS_ELF) $< -o $@

$(STAGE1_BIN): $(STAGE1_SRC) | $(BUILDDIR)
	@echo "Building Stage1..."
	@$(ASM) $(ASMFLAGS) $< -o $@

$(STAGE2_BIN): $(STAGE2_SRC) | $(BUILDDIR)
	@echo "Building Stage2..."
	@$(ASM) $(ASMFLAGS) $< -o $@

$(KERNEL_ENTRY_OBJ): $(KERNEL_ENTRY_SRC) | $(BUILDDIR)
	@echo "Assembling kernel entry..."
	@mkdir -p $(@D)
	@$(ASM) $(ASMFLAGS_ELF) $< -o $@

$(KERNEL_BIN): $(KERNEL_ENTRY_OBJ) $(C_OBJS) $(ASM_OBJS)
	@echo "Linking kernel (raw binary)..."
	@$(LD) $(LDFLAGS) -o $@ $^
	@echo "Kernel size: $$(stat -c%s $@) bytes (max: $(KERNEL_MAX_BYTES) bytes)"
	@if [ $$(stat -c%s $@) -gt $(KERNEL_MAX_BYTES) ]; then \
		echo "ERROR: Kernel exceeds $(KERNEL_SECTORS) sectors ($(KERNEL_MAX_BYTES) bytes)!"; \
		echo "Current size: $$(stat -c%s $@) bytes"; \
		exit 1; \
	fi

$(KERNEL_ELF): $(KERNEL_ENTRY_OBJ) $(C_OBJS) $(ASM_OBJS)
	@echo "Linking kernel ELF (with GCC)..."
	@$(LD) -g -nostdlib -T $(ENTRYDIR)/linker.ld -o $@ $^
#maybe     @$(CC) -g -nostdlib -T $(ENTRYDIR)/linker.ld -o $@ $^

# ==== DISK IMAGES ====
$(IMAGE): $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_BIN)
	@echo "Creating disk image (10MB)..."
	@dd if=/dev/zero of=$@ bs=512 count=20480 status=none
	@echo "  Writing Stage1 (sector 0, 512 bytes)..."
	@dd if=$(STAGE1_BIN) of=$@ bs=512 conv=notrunc status=none
	@echo "  Writing Stage2 (sectors 1-9, $(STAGE2_SECTORS) sectors)..."
	@dd if=$(STAGE2_BIN) of=$@ bs=512 seek=1 conv=notrunc status=none
	@echo "  Writing Kernel (sector $(KERNEL_START_SECTOR)+, $(KERNEL_SECTORS) sectors)..."
	@dd if=$(KERNEL_BIN) of=$@ bs=512 seek=$(KERNEL_START_SECTOR) conv=notrunc status=none
	@echo "Disk image created: $(IMAGE)"

$(FLOPPY_IMG): $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_BIN)
	@echo "Creating floppy image (1.44MB)..."
	@dd if=/dev/zero of=$@ bs=512 count=2880 status=none
	@dd if=$(STAGE1_BIN) of=$@ bs=512 conv=notrunc status=none
	@dd if=$(STAGE2_BIN) of=$@ bs=512 seek=1 conv=notrunc status=none
	@dd if=$(KERNEL_BIN) of=$@ bs=512 seek=$(KERNEL_START_SECTOR) conv=notrunc status=none
	@echo "Floppy image created: $(FLOPPY_IMG)"

$(ISO): $(FLOPPY_IMG)
	@echo "Creating ISO image..."
	@mkdir -p $(ISO_DIR)
	@cp $< $(ISO_DIR)/boot.img
	@xorriso -as mkisofs -b boot.img -no-emul-boot -boot-load-size 2880 -boot-info-table -o $@ $(ISO_DIR)

$(VBOX_VDI): $(IMAGE)
	@echo "Creating VirtualBox VDI..."
	@VBoxManage convertfromraw $< $@ --format VDI

# ==== UTILITIES ====
run: $(IMAGE)
	@echo "Running BoxOS in QEMU..."
	@$(QEMU) -drive format=raw,file=$< -m 512M -serial stdio -no-reboot -no-shutdown

runlog: $(IMAGE)
	@echo "Running BoxOS in QEMU with full logging..."
	@$(QEMU) -drive format=raw,file=$< -m 512M -serial stdio \
		-d int,cpu_reset -no-reboot -no-shutdown \
		-D boxos_qemu.log
	
CORES = 4
MEM = 4096M

rununiversal: $(IMAGE)
	@echo "Running BoxOS in QEMU with $(CORES) cores and $(MEM) memory size"
	@$(QEMU) -drive format=raw,file=$< -m $(MEM) -serial stdio \
		-smp $(CORES),cores=$(CORES),threads=1,sockets=1 \
		-d int,cpu_reset -no-reboot -no-shutdown \
		-D boxos_qemu.log


debug: $(IMAGE)
	@echo "Running BoxOS in QEMU with debugger..."
	@$(QEMU) -drive format=raw,file=$< -m 512M -serial stdio -s -S

clean:
	@echo "Cleaning build..."
	@rm -rf $(BUILDDIR)

install-deps:
	@echo "Installing dependencies..."
	@sudo apt update
	@sudo apt install -y nasm gcc binutils qemu-system-x86 xorriso virtualbox make

info:
	@echo $(QEMU) -drive format=raw,file=$(IMAGE) -m 512M -serial stdio -no-reboot -no-shutdown
	@echo "BoxOS Makefile Info"
	@echo "Targets:"
	@echo "  all        — full build (img, iso, elf)"
	@echo "  run        — run BoxOS in QEMU"
	@echo "  debug      — run QEMU with gdb waiting"
	@echo "  clean      — clean build directory"
	@echo "  install-deps — install required packages"

