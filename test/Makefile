CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O1 -g
SRC = ../ClearCoreStackLight/Light.cpp ../ClearCoreStackLight/Settings.cpp ../ClearCoreStackLight/Strip.cpp ../ClearCoreStackLight/Effects.cpp test_main.cpp

test: run_tests
	./run_tests

run_tests: $(SRC) ../ClearCoreStackLight/Light.h ../ClearCoreStackLight/Settings.h ../ClearCoreStackLight/Strip.h ../ClearCoreStackLight/Effects.h
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

clean:
	rm -f run_tests

.PHONY: test clean
