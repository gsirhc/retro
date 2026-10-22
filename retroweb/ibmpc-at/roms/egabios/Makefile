# EGA video BIOS stand-in. Needs nasm and the pinned fonts from ../fetch-bios.sh.

egabios.bin: egabios.asm font8.inc font14.inc
	nasm -f bin -o egabios.tmp egabios.asm
	python3 -c "import sys; d = bytearray(open('egabios.tmp', 'rb').read()); d[-1] = -sum(d[:-1]) & 0xFF; open('$@', 'wb').write(d)"
	rm -f egabios.tmp

font8.inc font14.inc: ../fntcol16.zip mkfont.py
	python3 mkfont.py ../fntcol16.zip

../fntcol16.zip:
	bash ../fetch-bios.sh

clean:
	rm -f egabios.bin egabios.tmp font8.inc font14.inc

.PHONY: clean
