# Programming 6502

## Links

Compiler: <http://www.compilers.de/vasm.html>

Programmer: <https://gitlab.com/DavidGriffith/minipro> (Install with brew)

## Setup

1. Unzip `vasm6502.zip` to .
2. Install minipro (see links above)

## Programming EEPROM

1. Create the assembly file (asm)
2. Compile the assembly:

   `./vasm6502_oldstyle/vasm6502_oldstyle test/ImAlive.asm -dotdir -Fbin`
3. Can check binary with:

   `hexdump -C a.out`
4. Send to programmer:

   `minipro -p AT28C256 -w a.out`
5. You can run:

   `sh compile.sh HellowWorld.asm`
