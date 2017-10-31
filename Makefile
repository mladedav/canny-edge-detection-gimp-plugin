CFLAGS='-Wall'
LDFLAGS='-lm'
G_MESSAGES_DEBUG='all'

test: 
	make install
	make run

run:
	G_MESSAGES_DEBUG=$(G_MESSAGES_DEBUG) gimp -a engine.png

lenna:
	G_MESSAGES_DEBUG=$(G_MESSAGES_DEBUG) gimp -a Lenna.png

install: canny.c
	CFLAGS=$(CFLAGS) LDFLAGS=$(LDFLAGS) gimptool-2.0 --install canny.c
