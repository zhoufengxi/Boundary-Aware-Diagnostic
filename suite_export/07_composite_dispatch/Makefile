CXX := clang++
CXXFLAGS ?= -std=c++17 -Wall -Wno-unused-variable -Wno-unused-private-field
SRCS := $(wildcard *.cpp)
OBJS := $(SRCS:.cpp=.o)
all: $(OBJS)
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -I. $(EXTRA_CXXFLAGS) -c $< -o $@
clean:
	rm -f $(OBJS)
.PHONY: all clean
