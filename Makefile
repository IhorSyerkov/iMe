# Firmware name and version
PROG = iMe
VERSION = 1900000001

# Tool locations
CC = /opt/avr-toolchain/bin/avr-gcc
COPY = /opt/avr-toolchain/bin/avr-objcopy
SIZE = /opt/avr-toolchain/bin/avr-size
M3DLINUX = /usr/sbin/m3d-linux

# Assembly source files
ASSRCS = src/ASF/xmega/drivers/cpu/ccp.s \
	src/ASF/xmega/drivers/nvm/nvm_asm.s

# C source files
CSRCS = src/ASF/common/boards/user_board/init.c \
	src/ASF/common/services/clock/xmega/sysclk.c \
	src/ASF/common/services/sleepmgr/xmega/sleepmgr.c \
	src/ASF/common/services/usb/class/cdc/device/udi_cdc.c \
	src/ASF/common/services/usb/class/cdc/device/udi_cdc_desc.c \
	src/ASF/common/services/usb/udc/udc.c \
	src/ASF/xmega/drivers/nvm/nvm.c \
	src/ASF/xmega/drivers/usb/usb_device.c \
	src/ASF/xmega/drivers/tc/tc.c \
	src/ASF/xmega/drivers/twi/twim.c

# C++ source files
CPPSRCS = main.cpp \
	gcode.cpp

# Include paths
INCPATH = . \
	src \
	src/config \
	src/ASF/common/boards \
	src/ASF/common/boards/user_board \
	src/ASF/common/services/clock \
	src/ASF/common/services/clock/xmega \
	src/ASF/common/services/sleepmgr \
	src/ASF/common/services/sleepmgr/xmega \
	src/ASF/common/services/usb \
	src/ASF/common/services/usb/class/cdc \
	src/ASF/common/services/usb/class/cdc/device \
	src/ASF/common/services/usb/udc \
	src/ASF/common/utils \
	src/ASF/common/utils/interrupt \
	src/ASF/xmega/drivers/cpu \
	src/ASF/xmega/drivers/nvm \
	src/ASF/xmega/drivers/sleep \
	src/ASF/xmega/drivers/usb \
	src/ASF/xmega/drivers/tc \
	src/ASF/xmega/drivers/twi \
	src/ASF/xmega/drivers/pmic \
	src/ASF/xmega/utils \
	src/ASF/xmega/utils/assembler \
	src/ASF/xmega/utils/bit_handling \
	src/ASF/xmega/utils/preprocessor

# Compiler flags
FLAGS = -D BOARD=USER_BOARD -D VERSION="\"$(VERSION)\"" -Os -mmcu=atxmega32c4 -Wall
ASFLAGS = -std=c++14 -x assembler-with-cpp
CFLAGS = -std=gnu99 -x c -fdata-sections -ffunction-sections -fpack-struct -fshort-enums -fno-strict-aliasing -Wstrict-prototypes -Wmissing-prototypes -Werror-implicit-function-declaration -Wpointer-arith -mrelax
CPPFLAGS = -std=c++14 -x c++ -funsigned-char -funsigned-bitfields -ffunction-sections -fdata-sections -fpack-struct -fshort-enums
LFLAGS = -Wl,--section-start=.BOOT=0x8000 -Wl,--start-group -Wl,--end-group -Wl,--gc-sections

# Make
all:
	$(CC) $(foreach INC, $(addprefix , $(INCPATH)), -I $(INC)) $(FLAGS) $(ASFLAGS) -c $(ASSRCS)
	$(CC) $(foreach INC, $(addprefix , $(INCPATH)), -I $(INC)) $(FLAGS) $(CFLAGS) -c $(CSRCS)
	$(CC) $(foreach INC, $(addprefix , $(INCPATH)), -I $(INC)) $(FLAGS) $(CPPFLAGS) -c $(CPPSRCS)
	$(CC) $(foreach INC, $(addprefix , $(INCPATH)), -I $(INC)) $(FLAGS) $(LFLAGS) *.o -o $(PROG).elf
	@$(COPY) -O binary $(PROG).elf "$(PROG) $(VERSION).hex"
	@$(SIZE) --mcu=atxmega32c4 -C $(PROG).elf
	@rm -f *.o $(PROG).elf
	@echo $(PROG) $(VERSION).hex is ready

# Make clean
clean:
	rm -f $(PROG).elf "$(PROG) $(VERSION).hex" *.o

# Make run
run:
	@$(M3DLINUX) -a -x -r "$(PROG) $(VERSION).hex"
