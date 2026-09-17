all: world

CXX?=g++
CXXFLAGS?=--std=c++17 -Wall -fPIC -g

INCLUDES+= \
	-I./include \
	-I./rva/include \
	-I./tsl/include

OBJS:= \
	objs/main.o

COMMON_DIR:=./common
LOGGER_DIR:=./logger
USAGECPP_DIR:=./usage
UCI_DIR:=./uci
NETLINK_DIR:=./netlink

include ./common/Makefile.inc
include ./logger/Makefile.inc
include ./usage/Makefile.inc
include ./uci/Makefile.inc
include ./netlink/Makefile.inc

world: tcpredir

$(shell mkdir -p objs)

objs/main.o: src/main.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

tcpredir: $(COMMON_OBJS) $(LOGGER_OBJS) $(USAGE_OBJS) $(UCI_OBJS) $(NETLINK_OBJS) $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@;

.PHONY: clean
clean:
	@rm -rf objs tcpredir
