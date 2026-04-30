#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#

PROJECT_NAME := receiver-8266

# ESP8266_RTOS_SDK snapshots the current CC into HOSTCC before it swaps CC to
# the xtensa toolchain. Point that early host compiler at our wrapper so
# kconfig/menuconfig can link libintl on Alpine musl hosts.
CC := $(CURDIR)/tools/hostcc

include $(IDF_PATH)/make/project.mk

