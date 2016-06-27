CC=gcc
LDFLAG = -lm

ping: ping.o
	$(CC) -o ping ping.o $(LDFLAG)

clean:
	rm ping *.o
