CC := $(HOME)/opt/cross-x86_64-elf/bin/x86_64-elf-gcc
AS := nasm

CFLAGS := -ffreestanding -fno-stack-protector -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel -Wall -Wextra -std=gnu11 -O2 -Ikernel
ASFLAGS := -f elf64

BUILD_DIR := build
ISO_DIR := iso

C_SOURCES := $(wildcard kernel/*.c) $(wildcard kernel/mm/*.c)
C_OBJECTS := $(patsubst kernel/%.c,$(BUILD_DIR)/%.o,$(C_SOURCES))
ASM_OBJECTS := $(BUILD_DIR)/boot.o $(BUILD_DIR)/gdt_flush.o $(BUILD_DIR)/isr_stubs.o $(BUILD_DIR)/context_switch.o $(BUILD_DIR)/syscall_entry.o

.PHONY: all build iso run clean disk-image

all: build

build: $(BUILD_DIR)/kernel.elf

$(BUILD_DIR)/boot.o: boot/boot.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) boot/boot.asm -o $(BUILD_DIR)/boot.o

$(BUILD_DIR)/gdt_flush.o: kernel/gdt_flush.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) kernel/gdt_flush.asm -o $(BUILD_DIR)/gdt_flush.o

$(BUILD_DIR)/isr_stubs.o: kernel/isr.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) kernel/isr.asm -o $(BUILD_DIR)/isr_stubs.o

$(BUILD_DIR)/context_switch.o: kernel/context_switch.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) kernel/context_switch.asm -o $(BUILD_DIR)/context_switch.o

$(BUILD_DIR)/syscall_entry.o: kernel/syscall_entry.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) kernel/syscall_entry.asm -o $(BUILD_DIR)/syscall_entry.o

$(BUILD_DIR)/%.o: kernel/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel.elf: $(ASM_OBJECTS) $(C_OBJECTS) linker.ld
	$(CC) -T linker.ld -o $(BUILD_DIR)/kernel.elf -ffreestanding -O2 -nostdlib $(ASM_OBJECTS) $(C_OBJECTS) -lgcc

iso: $(BUILD_DIR)/kernel.elf
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(BUILD_DIR)/kernel.elf $(ISO_DIR)/boot/kernel.elf
	cp boot/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o $(BUILD_DIR)/neoos.iso $(ISO_DIR)

DISK_IMG := $(BUILD_DIR)/disk.img
DISK_SRC := $(BUILD_DIR)/disk-src

USERLAND_DIR := userland
USERLAND_BUILD := $(BUILD_DIR)/userland
USER_CFLAGS := -ffreestanding -fno-stack-protector -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=large -fno-pic -static -nostdlib -Wall -Wextra -std=gnu11 -O2 -I$(USERLAND_DIR)

$(USERLAND_BUILD)/SPIN.ELF: $(USERLAND_DIR)/spin.c $(USERLAND_DIR)/user.ld $(USERLAND_DIR)/neoos_syscall.h
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(USERLAND_DIR)/spin.c

$(USERLAND_BUILD)/CHILD.ELF: $(USERLAND_DIR)/child.c $(USERLAND_DIR)/user.ld $(USERLAND_DIR)/neoos_syscall.h
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(USERLAND_DIR)/child.c

$(USERLAND_BUILD)/PARENT.ELF: $(USERLAND_DIR)/parent.c $(USERLAND_DIR)/user.ld $(USERLAND_DIR)/neoos_syscall.h
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(USERLAND_DIR)/parent.c

$(DISK_IMG): $(USERLAND_BUILD)/SPIN.ELF $(USERLAND_BUILD)/CHILD.ELF $(USERLAND_BUILD)/PARENT.ELF
	mkdir -p $(DISK_SRC)/DIR
	printf 'Hello from NeoOS FAT16!\n' > $(DISK_SRC)/HELLO.TXT
	head -c 8192 /dev/zero | tr '\0' 'N' > $(DISK_SRC)/BIGFILE.TXT
	printf 'nested file contents\n' > $(DISK_SRC)/DIR/NESTED.TXT
	dd if=/dev/zero of=$(DISK_IMG) bs=1M count=32 status=none
	mkfs.fat -F 16 $(DISK_IMG)
	mcopy -i $(DISK_IMG) $(DISK_SRC)/HELLO.TXT ::HELLO.TXT
	mcopy -i $(DISK_IMG) $(DISK_SRC)/BIGFILE.TXT ::BIGFILE.TXT
	mmd -i $(DISK_IMG) ::DIR
	mcopy -i $(DISK_IMG) $(DISK_SRC)/DIR/NESTED.TXT ::DIR/NESTED.TXT
	mmd -i $(DISK_IMG) ::BIN
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/SPIN.ELF ::BIN/SPIN.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/CHILD.ELF ::BIN/CHILD.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/PARENT.ELF ::BIN/PARENT.ELF

disk-image: $(DISK_IMG)

run: iso disk-image
	qemu-system-x86_64 -boot order=d -cdrom $(BUILD_DIR)/neoos.iso -drive file=$(DISK_IMG),format=raw

clean:
	rm -rf $(BUILD_DIR) $(ISO_DIR)
