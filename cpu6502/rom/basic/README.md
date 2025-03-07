COMPILER (BIOS)
===============

https://github.com/cc65/cc65
make (takes FOREVER!)

Generate .o file
cc65/bin/ld65 bios.s

Generate a.out file with symbols:
cc65/bin/ld65 -C bios.cfg bios.o -Ln bios.sym

Add memory addresses to bios.cfg

Write to ROM chip:
minipro -p AT28C256 -w a.out


MS BASIC
========

git clone https://github.com/mist64/msbasic.git
make.sh

defines_gall_oac.s - memory addresses
gall_oac.cfg - memory map

compiles to ./tmp/gall_oac*

minipro -p AT28C256 -w tmp/gall_oac.bin

Search GALL_OAC for all the customizations and configs

Custom commands in GALL_OAC in tokens.s
