CC      = gcc
LD      = ld
NASM    = nasm
DATA_LBA = 2048

CFLAGS  = -m32 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -fcf-protection=none \
          -mgeneral-regs-only -O2 -Wall -Wextra -fno-asynchronous-unwind-tables \
          -nostdlib -DDATA_LBA=$(DATA_LBA)

B = build

all: $(B)/base.img

$(B):
	mkdir -p $(B)

$(B)/entry.o: kernel/entry.asm | $(B)
	$(NASM) -f elf32 $< -o $@

$(B)/%.o: kernel/%.c kernel/kernel.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

OBJS = $(B)/entry.o $(B)/kernel.o $(B)/hda.o $(B)/ac97.o

$(B)/kernel.elf: $(OBJS) kernel/linker.ld
	$(LD) -m elf_i386 -T kernel/linker.ld --build-id=none -z noexecstack -o $@ $(OBJS)

$(B)/kernel.bin: $(B)/kernel.elf
	objcopy -O binary $< $@
	@test $$(stat -c%s $@) -lt 262144 || (echo "kernel demasiado grande" && false)

$(B)/stage2.bin: boot/stage2.asm | $(B)
	$(NASM) -f bin $< -o $@

$(B)/boot.bin: boot/boot.asm $(B)/stage2.bin $(B)/kernel.bin
	$(NASM) -f bin -DKSECT=$$(( ($$(stat -c%s $(B)/stage2.bin) + $$(stat -c%s $(B)/kernel.bin) + 511) / 512 )) $< -o $@

$(B)/base.img: $(B)/boot.bin $(B)/stage2.bin $(B)/kernel.bin
	cat $(B)/boot.bin $(B)/stage2.bin $(B)/kernel.bin > $@
	truncate -s $$(( $(DATA_LBA) * 512 )) $@

# Video de prueba sintetico (8 s) para verificar sin tener Bad Apple a la mano
$(B)/demo.mp4: | $(B)
	ffmpeg -v error -y -f lavfi -i "testsrc2=size=160x100:rate=30:duration=8" \
	       -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=8" \
	       -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest $@

demo: $(B)/base.img $(B)/demo.mp4
	python3 tools/mkbad.py $(B)/base.img $(B)/demo.mp4 -o $(B)/demo.img

# make bad VIDEO=bad_apple.mp4  ->  build/badkernel.img
bad: $(B)/base.img
	python3 tools/mkbad.py $(B)/base.img $(VIDEO) -o $(B)/badkernel.img

QEMU_AUDIO ?= pipewire
QEMU_FLAGS ?= -enable-kvm
run: build/badkernel.img
	qemu-system-i386 $(QEMU_FLAGS) -drive file=build/badkernel.img,format=raw \
	  -audiodev $(QEMU_AUDIO),id=snd0 -device intel-hda -device hda-output,audiodev=snd0 -serial stdio

run-ac97: build/badkernel.img
	qemu-system-i386 $(QEMU_FLAGS) -drive file=build/badkernel.img,format=raw \
	  -audiodev $(QEMU_AUDIO),id=snd0 -device AC97,audiodev=snd0 -serial stdio

clean:
	rm -rf $(B)

.PHONY: all demo bad run run-ac97 clean
