CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O1 -g
S = ../ClearCoreStackLight
SRC = $(S)/Breadcrumb.cpp $(S)/Light.cpp $(S)/Settings.cpp $(S)/Strip.cpp $(S)/Effects.cpp test_main.cpp
HDR = $(S)/Breadcrumb.h $(S)/Light.h $(S)/Settings.h $(S)/Strip.h $(S)/Effects.h

test: run_tests
	./run_tests

run_tests: $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

clean:
	rm -f run_tests

.PHONY: test clean
