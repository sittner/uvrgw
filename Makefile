CC ?= gcc

TARGET = uvrgw

SRC = \
	main.c \
	utils.c \
	uvrgw_conf.c \
	can.c \
	mb.c \
	mqtt.c \
	mqtt_logger.c \
	rest.c \
	ntp_check.c \
	sunspec.c \
	counter.c \
	uvlua.c \

OBJ = $(SRC:.c=.o)

CFLAGS += -DGCC_COMPILER

CFLAGS += -I.

CFLAGS += -Wall

CFLAGS += -g

# separate variable: not overridden by CFLAGS given on the command line
LUA_CFLAGS = $(shell pkg-config --cflags lua5.4)

LIBS += -lpthread -lconfuse -lmodbus -lmosquitto -lcurl -ljson-c -lm
LIBS += $(shell pkg-config --libs lua5.4)

.PHONY: all clean realclean install

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $(TARGET) $(OBJ) $(LIBS)

%.o: %.c
	$(CC) -c $(CFLAGS) $(LUA_CFLAGS) -o $@ $< 

clean:
	rm -f $(OBJ)
	rm -f $(TARGET)

install: $(TARGET)
	install -m755 -D $(TARGET) $(DESTDIR)/usr/bin/$(TARGET)
	install -m644 -D uvrgw.service $(DESTDIR)/lib/systemd/system/uvrgw.service

realclean: clean
	make -C test clean

