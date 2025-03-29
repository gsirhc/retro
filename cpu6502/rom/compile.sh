cd msbasic_galloac
rm tmp/gall_oac.bin
./make.sh
echo "========="
minipro -p AT28C256 -w tmp/gall_oac.bin
