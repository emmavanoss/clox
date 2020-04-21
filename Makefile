CC=gcc # compiler
TARGET=main # target file name

all:
	$(CC) *.c -o $(TARGET)
	./main

run:
	$(CC) *.c -o $(TARGET)
	./main scratch.lox

clean:
	rm $(TARGET)
