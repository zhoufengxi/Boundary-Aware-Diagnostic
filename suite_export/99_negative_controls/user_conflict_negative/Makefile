CXX := clang++
CXXFLAGS ?= -std=c++17 -Wall -Wno-unused-variable -Wno-unused-private-field
all: manager.o worker.o
manager.o: manager.cpp
	$(CXX) $(CXXFLAGS) -I. $(EXTRA_CXXFLAGS) -c $< -o $@
worker.o: worker.cpp
	$(CXX) $(CXXFLAGS) -I. $(EXTRA_CXXFLAGS) -c $< -o $@
clean:
	rm -f *.o
.PHONY: all clean
