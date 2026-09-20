all: world

CXX?=g++
CXXFLAGS?=--std=c++20 -Wall -fPIC -g

INCLUDES+= \
	-I./include \
	-I./rva/include \
	-I./tsl/include

# ubus/Makefile.inc already provides -lubox -lblobmsg_json -lubus, in that order.

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

# Libraries go AFTER the objects. An --as-needed toolchain (Alpine, which the CI
# builds on) drops a -l that resolves nothing undefined *at the point it appears*,
# so libs listed first are discarded and every ubus/uloop symbol comes back
# undefined at the end of the link. GNU ld resolves left to right.

$(shell mkdir -p objs)

objs/main.o: src/main.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c -o $@ $<;

# Without this, main.o depends on main.cpp alone: editing version.hpp (or any
# other header) leaves a stale object behind, and the build quietly produces a
# binary that reports the previous version.
-include objs/main.d

tcpredir: $(COMMON_OBJS) $(LOGGER_OBJS) $(USAGE_OBJS) $(JSON_OBJS) $(UCI_OBJS) $(UBUS_OBJS) $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LIBS) -o $@;

.PHONY: clean
clean:
	@rm -rf objs tcpredir
