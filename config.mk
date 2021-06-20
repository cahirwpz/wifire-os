# vim: tabstop=8 shiftwidth=8 noexpandtab:
#
# Common makefile used to establish values for variables that configure
# build system for given platform.
#

CONFIG_OPTS := KASAN LOCKDEP KGPROF MIPS AARCH64 KCSAN

BOARD ?= malta

MIPS ?= 0
AARCH64 ?= 0

ifeq ($(BOARD), malta)
ARCH := mips
MIPS := 1
endif

ifeq ($(BOARD), rpi3)
ARCH := aarch64
AARCH64 := 1
endif

VERBOSE ?= 0
CLANG ?= 0
LOCKDEP ?= 0
KASAN ?= 1
KGPROF ?= 0
KCSAN ?= 0
