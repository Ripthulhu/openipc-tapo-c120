CFLAGS ?= -Os -Wall -Wpedantic
LDLIBS += -lcjson -lm

all: xmdp

xmdp: xmdp.o netip.o utils.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

clean:
	-rm xmdp *.o
