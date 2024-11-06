CC ?= gcc

TARGET = uvrgw

SRC = \
	main.c \
	utils.c \
	uvrgw_conf.c \
	can.c \
	mb.c \
	mqtt.c \
	rest.c \
	ntp_check.c \

OBJ = $(SRC:.c=.o)

CFLAGS += -DGCC_COMPILER

CFLAGS += -I.

CFLAGS += -Wall

CFLAGS += -g

LIBS += -lpthread -lconfuse -lmodbus -lmosquitto -lcurl -ljson-c -lm

.PHONY: all clean realclean install

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $(TARGET) $(OBJ) $(LIBS)

%.o: %.c
	$(CC) -c $(CFLAGS) -o $@ $< 

clean:
	rm -f $(OBJ)
	rm -f $(TARGET)

install: $(TARGET)
	install -m755 -D $(TARGET) $(DESTDIR)/usr/bin/$(TARGET)

realclean: clean
	make -C test clean

