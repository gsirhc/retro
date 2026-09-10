cd "$(dirname "$0")"

if [ ! -d tmp ]; then
	mkdir tmp
fi

ca65 --cpu 65C02 bios.s -o tmp/firmware.o &&
ld65 -C link.cfg tmp/firmware.o -o tmp/firmware.bin -Ln tmp/firmware.lbl
