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
	eval.c \
	tinyexpr/tinyexpr.c \

OBJ = $(SRC:.c=.o)

CFLAGS += -DGCC_COMPILER

CFLAGS += -I.

CFLAGS += -Wall

CFLAGS += -g

# not part of CFLAGS, so they are kept if CFLAGS is given on the command line
INCLUDES = -Itinyexpr

# tinyexpr options (tinyexpr.c stays unmodified, see tinyexpr/README.uvrgw)
tinyexpr/tinyexpr.o: DEFINES += -DTE_POW_FROM_RIGHT

LIBS += -lpthread -lconfuse -lmodbus -lmosquitto -lcurl -ljson-c -lm

.PHONY: all clean realclean install

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $(TARGET) $(OBJ) $(LIBS)

%.o: %.c
	$(CC) -c $(CFLAGS) $(DEFINES) $(INCLUDES) -o $@ $<

clean:
	rm -f $(OBJ)
	rm -f $(TARGET)

install: $(TARGET)
	install -m755 -D $(TARGET) $(DESTDIR)/usr/bin/$(TARGET)
	install -m644 -D uvrgw.service $(DESTDIR)/lib/systemd/system/uvrgw.service

realclean: clean
	make -C test clean

