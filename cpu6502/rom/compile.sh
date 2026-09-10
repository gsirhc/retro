cd "$(dirname "$0")"
rm -f tmp/firmware.bin
./make.sh
echo "========="
minipro -p AT28C256 -w tmp/firmware.bin
