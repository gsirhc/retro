cd msbasic_galloac

if [ ! -d tmp ]; then
	mkdir tmp
fi

$i = gall_oac

echo $i
ca65 -D $i msbasic.s -o tmp/$i.o &&
ld65 -C ../$i.cfg tmp/$i.o -o tmp/$i.bin -Ln tmp/$i.lbl

done

