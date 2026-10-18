# Makefile - build no Linux/WSL (no Windows use build.ps1).
#   make          -> out/orion.elf + out/esp/
#   make run      -> QEMU (UEFI, boot pela pasta out/esp)
#   make iso      -> out/orion.iso (hibrida, BIOS + UEFI, com o tools/mkiso.c)
#   make run-iso  -> QEMU bootando a ISO em modo BIOS
#   make clean

KERNEL   := orion
CC       := clang
LD       := ld.lld
QEMU     := qemu-system-x86_64
LIMINE   := third_party/limine

SRC_DIR   := kernel/src
BUILD_DIR := build
OUT_DIR   := out

CFLAGS := --target=x86_64-unknown-none-elf -std=gnu11 -O2 -g -Wall -Wextra \
    -ffreestanding -nostdlibinc -fno-stack-protector -fno-stack-check -fno-lto \
    -fno-pic -fno-pie -ffunction-sections -fdata-sections \
    -m64 -march=x86-64 -mabi=sysv -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
    -mno-red-zone -mcmodel=kernel \
    -Ikernel/include -I$(LIMINE) -Ithird_party/uacpi/include -MMD -MP

LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
    -z noexecstack --gc-sections -T linker.ld

SRCS := $(shell find $(SRC_DIR) -name '*.c' -o -name '*.S')
UACPI_SRCS := $(wildcard third_party/uacpi/source/*.c)
OBJS := $(patsubst $(SRC_DIR)/%,$(BUILD_DIR)/%.o,$(SRCS)) \
    $(patsubst third_party/uacpi/source/%,$(BUILD_DIR)/uacpi/%.o,$(UACPI_SRCS))
DEPS := $(OBJS:.o=.d)

ESP := $(OUT_DIR)/esp
ELF := $(OUT_DIR)/$(KERNEL).elf

OVMF_CODE := $(firstword $(wildcard /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/edk2/x64/OVMF_CODE.fd /usr/share/edk2-ovmf/x64/OVMF_CODE.fd))
OVMF_VARS := $(firstword $(wildcard /usr/share/OVMF/OVMF_VARS_4M.fd /usr/share/OVMF/OVMF_VARS.fd /usr/share/edk2/x64/OVMF_VARS.fd /usr/share/edk2-ovmf/x64/OVMF_VARS.fd))
QEMUFLAGS ?= -M q35 -m 512M -serial stdio -no-reboot -no-shutdown

.PHONY: all esp run iso run-iso clean user

all: esp

user:
	python3 tools/musl_build.py

RUST_LIB := $(BUILD_DIR)/rust/x86_64-unknown-none/release/liborion_rust.a
RUST_SRCS := $(shell find kernel/rust -name '*.rs' -o -name 'Cargo.toml')

$(RUST_LIB): $(RUST_SRCS)
	RUSTFLAGS="-C code-model=kernel -C relocation-model=static -D warnings" cargo build --release --quiet 		--target x86_64-unknown-none --manifest-path kernel/rust/orion/Cargo.toml --target-dir $(BUILD_DIR)/rust

$(ELF): $(OBJS) $(RUST_LIB) linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(RUST_LIB)

$(BUILD_DIR)/%.c.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/uacpi/%.c.o: third_party/uacpi/source/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.S.o: $(SRC_DIR)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

esp: $(ELF) limine.conf
	@mkdir -p $(ESP)/EFI/BOOT $(ESP)/boot/limine $(ESP)/bin $(ESP)/dev $(ESP)/mnt $(ESP)/tmp $(ESP)/run $(ESP)/proc
	cp $(LIMINE)/BOOTX64.EFI $(ESP)/EFI/BOOT/
	cp $(ELF) $(ESP)/boot/$(KERNEL).elf
	rm -f $(ESP)/boot/*.elf.bak
	python3 tools/musl_build.py
	cp limine.conf $(ESP)/boot/limine/

run: esp
	@test -n "$(OVMF_CODE)" || (echo "OVMF nao encontrado (apt install ovmf)"; exit 1)
	@test -f $(OUT_DIR)/ovmf-vars.fd || cp $(OVMF_VARS) $(OUT_DIR)/ovmf-vars.fd
	$(QEMU) $(QEMUFLAGS) \
		-drive if=pflash,format=raw,unit=0,file=$(OVMF_CODE),readonly=on \
		-drive if=pflash,format=raw,unit=1,file=$(OUT_DIR)/ovmf-vars.fd \
		-drive format=raw,file=fat:rw:$(ESP)

iso: esp
	python3 -c "import sys; sys.path.insert(0, 'tools'); import qrun; sys.exit(qrun.make_iso().returncode)"

run-iso: iso
	$(QEMU) $(QEMUFLAGS) -cdrom $(OUT_DIR)/$(KERNEL).iso -boot d

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)

-include $(DEPS)
