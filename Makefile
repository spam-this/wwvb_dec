wwvb_dec: wwvb_dec.c
	gcc -O -g -o wwvb_dec wwvb_dec.c -lpigpio -lpthread

clean:
	\rm -f wwvb_dec
