CC      = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy

PROJECT_ROOT := ../..

MCU     = -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
DEFS    = -DSTM32F411xE

CFLAGS  = $(MCU) -O0 -g -Wall -ffreestanding $(DEFS) -u _printf_float
LDFLAGS = -T $(PROJECT_ROOT)/linker.ld -nostdlib -lc -lm -lgcc -lnosys


INCLUDES = \
 -I$(PROJECT_ROOT)/cmsis/Include \
 -I$(PROJECT_ROOT)/include \
 -I$(PROJECT_ROOT)/include/freertos \
 -Ifreertos/portable/GCC/ARM_CM4F

SRC = \
 main.c \
 usart_debug.c \
 i2c.c \
 spi2.c \
 w5500.c \
 gps.c \
 delay.c \
 system_stm32f4xx.c \
 startup.s \
 syscalls.c

FREERTOS_SRC = \
 freertos/list.c \
 freertos/queue.c \
 freertos/tasks.c \
 freertos/timers.c \
 freertos/stream_buffer.c \
 freertos/portable/GCC/ARM_CM4F/port.c \
 freertos/portable/MemMang/heap_4.c

OBJ = $(SRC:.c=.o)
OBJ := $(OBJ:.s=.o)
OBJ += $(FREERTOS_SRC:.c=.o)

TARGET = firmware.elf
BIN    = firmware.bin

all: $(TARGET) $(BIN)

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

%.o: %.s
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) $(LDFLAGS) -o $@

$(BIN): $(TARGET)
	$(OBJCOPY) -O binary $< $@

flash:
	openocd -f board/st_nucleo_f4.cfg \
	        -c "program $(TARGET) verify reset exit"

clean:
	rm -f *.o freertos/*.o freertos/portable/GCC/ARM_CM4F/*.o freertos/portable/MemMang/*.o *.elf *.bin

serialmonitor:
	minicom -b 115200 -D /dev/ttyACM0

.PHONY: all clean flash serialmonitor