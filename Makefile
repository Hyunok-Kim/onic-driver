MOD_NAME := onic
ONIC_DRV_HOME := $(shell pwd)
ONIC_DRV_KVER := $(shell uname -r)

SRC_FOLDERS = . libqdma/qdma_access libqdma \
              libqdma/qdma_access/qdma_soft_access \
              libqdma/qdma_access/eqdma_soft_access \
              libqdma/qdma_access/qdma_s80_hard_access \
			  jsmn 

ifneq ($(SUBDIRS),)
    ONIC_OBJS = $(foreach CURR, $(SRC_FOLDERS), $(patsubst $(SUBDIRS)/$(CURR)/%.c, $(CURR)/%.o, $(wildcard $(SUBDIRS)/$(CURR)/*.c)))
    EXTRA_CFLAGS = $(foreach CURR, $(SRC_FOLDERS), -I$$SUBDIRS/$(CURR))
endif

EXTRA_CFLAGS += -DMBOX_INTERRUPT_DISABLE
#CFLAGS_./onic_main.o := -DDEBUG

obj-m += $(MOD_NAME).o
$(MOD_NAME)-objs += $(ONIC_OBJS)

ccflags-y := -Wall

all:
	make -C /lib/modules/$(ONIC_DRV_KVER)/build M=$(ONIC_DRV_HOME) SUBDIRS=$(shell pwd) modules

clean:
	make -C /lib/modules/$(ONIC_DRV_KVER)/build M=$(ONIC_DRV_HOME) SUBDIRS=$(shell pwd) clean
	rm -f libqdma/*.o.ur-safe *.o.ur-safe

install:
	install -d ${MODULES_INSTALL_PATH}
	install -t ${MODULES_INSTALL_PATH} $(MOD_NAME).ko

json_install:
	install -d /lib/firmware/xilinx/
	install -m 644 json/*.json /lib/firmware/xilinx/
