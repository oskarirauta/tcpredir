all: world

CXX?=g++
CXXFLAGS?=--std=c++20 -Wall -fPIC -g

INCLUDES+= \
	-I./include \
	-I./rva/include \
	-I./tsl/include

LIBS+= \
	-lubus

OBJS:= \
	objs/main.o

COMMON_DIR:=./common
LOGGER_DIR:=./logger
USAGECPP_DIR:=./usage
JSON_DIR:=./json
UCI_DIR:=./uci
UBUS_DIR:=./ubus

include ./common/Makefile.inc
include ./logger/Makefile.inc
include ./usage/Makefile.inc
include ./json/Makefile.inc
include ./uci/Makefile.inc
include ./ubus/Makefile.inc

world: tcpredir

$(shell mkdir -p objs)

objs/main.o: src/main.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

tcpredir: $(COMMON_OBJS) $(LOGGER_OBJS) $(USAGE_OBJS) $(JSON_OBJS) $(UCI_OBJS) $(UBUS_OBJS) $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(LIBS) $^ -o $@;

.PHONY: clean
clean:
	@rm -rf objs tcpredir
