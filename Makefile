
all: wifiswd

wifiswd: wifiswd.c
	gcc -Wall -O2 wifiswd.c -o wifiswd -lutil

install: wifiswd
	cp wifiswd /usr/local/bin/

uninstall:
	rm -f /usr/local/bin/wifiswd

clean:
	rm -f wifiswd
