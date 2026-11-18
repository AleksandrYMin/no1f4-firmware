# No.1 F4 — собственное приложение (nRF51422, S110 v8.0.0, приложение с 0x18000).
# Сборка из Git Bash:  make        -> build/f4app.bin, build/f4app_dfu.zip
#                      make flash  -> собрать и прошить по BLE (tools/f4dfu.py)
#                      make clean

ROOT     := ..
PREFIX   ?= $(ROOT)/toolchain/gcc-arm/bin/arm-none-eabi-
SDK      ?= $(ROOT)/toolchain/nrf5_sdk_10/components
PYTHON   ?= python
F4GFX    := $(ROOT)/original/tools/f4gfx.py

TARGET   := f4app
BUILD    := build

CC       := $(PREFIX)gcc
OBJCOPY  := $(PREFIX)objcopy
SIZE     := $(PREFIX)size

SRC_C    := src/main.c src/display.c src/sensors.c src/extflash.c src/hid.c \
            $(SDK)/toolchain/system_nrf51.c $(SDK)/drivers_nrf/delay/nrf_delay.c \
            $(SDK)/libraries/bootloader_dfu/bootloader_util.c
SRC_S    := $(SDK)/toolchain/gcc/gcc_startup_nrf51.s

INC      := -I$(SDK)/softdevice/s110/headers -I$(SDK)/device \
            -I$(SDK)/toolchain/gcc -I$(SDK)/toolchain -I$(SDK)/drivers_nrf/delay \
            -I$(SDK)/libraries/bootloader_dfu

DEFS     := -DNRF51 -DS110 -DSOFTDEVICE_PRESENT -DBLE_STACK_SUPPORT_REQD

ARCH     := -mcpu=cortex-m0 -mthumb -mabi=aapcs -mfloat-abi=soft
# --short-enums обязателен: так собран интерфейс SoftDevice.
CFLAGS   := $(ARCH) $(DEFS) $(INC) -std=gnu99 -Os -g3 -Wall \
            -ffunction-sections -fdata-sections -fno-strict-aliasing \
            -fno-builtin --short-enums
ASFLAGS  := $(ARCH) $(DEFS) -x assembler-with-cpp -D__HEAP_SIZE=0 -D__STACK_SIZE=2048
LDFLAGS  := $(ARCH) -L$(SDK)/toolchain/gcc -Tnrf51_s110_app.ld \
            -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map \
            -Wl,--no-enum-size-warning --specs=nano.specs -nostartfiles

OBJS     := $(addprefix $(BUILD)/,$(notdir $(SRC_C:.c=.o) $(SRC_S:.s=.o)))
vpath %.c $(sort $(dir $(SRC_C)))
vpath %.s $(sort $(dir $(SRC_S)))

all: $(BUILD)/$(TARGET)_dfu.zip

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.s | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS) nrf51_s110_app.ld
	$(CC) $(LDFLAGS) $(OBJS) -o $@
	$(SIZE) $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

# DFU-пакет в том же формате, что заводской (CRC16, sd_req 0x0064).
$(BUILD)/$(TARGET)_dfu.zip: $(BUILD)/$(TARGET).bin
	$(PYTHON) $(F4GFX) pack $< -o $@

# Прошивка по BLE со встроенного Bluetooth компьютера (браслет сам уйдёт в DFU).
flash: $(BUILD)/$(TARGET)_dfu.zip
	$(PYTHON) tools/f4dfu.py $<

clean:
	rm -rf $(BUILD)

.PHONY: all flash clean
