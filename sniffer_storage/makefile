CC = gcc

CFLAGS = -I.. -DSNIFFER_STORAGE_COMPILE


LDFLAGS = -lusb-1.0

TARGETS = sniffer_storage.exe pc_to_uwb.exe twoway_transport.exe

all: $(TARGETS)

sniffer_storage.exe: sniffer_storage.c
	$(CC) $(CFLAGS) -o sniffer_storage.exe sniffer_storage.c $(LDFLAGS)

pc_to_uwb.exe: pc_to_uwb.c
	$(CC) $(CFLAGS) -o pc_to_uwb.exe pc_to_uwb.c $(LDFLAGS)

twoway_transport.exe: twoway_transport.c
	$(CC) $(CFLAGS) -o twoway_transport.exe twoway_transport.c $(LDFLAGS) -pthread

clean:
	rm -f $(TARGETS) sniffer_storage pc_to_uwb twoway_transport
