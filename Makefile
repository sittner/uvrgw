CC ?= gcc

TARGET = uvrgw

SRC = \
	main.c \
	ioconf.c \
	can.c \
	timer.c \
	mb.c \
	mqtt.c \
	rest.c \
	ntp_check.c \

OBJ = $(SRC:.c=.o)

CFLAGS += -DGCC_COMPILER

CFLAGS += -I.

CFLAGS += -Wall

LIBS += -lmodbus -lmosquitto -lcurl -ljson-c

.PHONY: all clean realclean install

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $(TARGET) $(OBJ) $(DBUS_OBJ) $(LIBS)

%.o: %.c
	$(CC) -c $(CFLAGS) -o $@ $< 

clean:
	rm -f $(OBJ)
	rm -f $(TARGET)

install: $(TARGET)
	install -m755 -D $(TARGET) $(DESTDIR)/usr/bin/$(TARGET)

realclean: clean
	make -C test clean

