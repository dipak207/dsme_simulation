CXX ?= c++
CXXFLAGS ?= -std=c++11 -Wall -Wextra -Werror -pedantic

TARGET = adaptive_mo_test

.PHONY: all test clean

all: $(TARGET)

$(TARGET): adaptive_mo_test.cc ../../core/net/mac/dsme/AdaptiveMOController.h ../../core/net/mac/dsme/openDSME/dsmeLayer/beaconManager/MOProtocol.h
	$(CXX) $(CXXFLAGS) -o $@ adaptive_mo_test.cc

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
