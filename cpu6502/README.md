# Programming 6502

## Links

Compiler: http://www.compilers.de/vasm.html

Programmer: https://gitlab.com/DavidGriffith/minipro (Install with brew)

## Programming EEPROM

1. Create the assembly

2. Compile the assembly:

   `./vasm6502_oldstyle/vasm6502_oldstyle HelloWorld.asm -dotdir -Fbin`

3. Can check binary with:

   `hexdump -C a.out`

4. Send to programmer

   `minipro -p AT28C256 -w a.out`

