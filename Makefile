CC := $(HOME)/opt/cross-x86_64-elf/bin/x86_64-elf-gcc
AS := nasm

CFLAGS := -ffreestanding -fno-stack-protector -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -Wall -Wextra -std=gnu11 -O2 -Ikernel
ASFLAGS := -f elf64

BUILD_DIR := build
ISO_DIR := iso

C_SOURCES := $(wildcard kernel/*.c) $(wildcard kernel/mm/*.c)
C_OBJECTS := $(patsubst kernel/%.c,$(BUILD_DIR)/%.o,$(C_SOURCES))
ASM_OBJECTS := $(BUILD_DIR)/boot.o $(BUILD_DIR)/gdt_flush.o $(BUILD_DIR)/isr_stubs.o

.PHONY: all build iso run clean

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

run: iso
	qemu-system-x86_64 -cdrom $(BUILD_DIR)/neoos.iso

clean:
	rm -rf $(BUILD_DIR) $(ISO_DIR)
