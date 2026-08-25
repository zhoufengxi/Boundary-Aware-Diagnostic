CXX := clang++
CXXFLAGS ?= -std=c++17 -Wall -Wno-unused-variable -Wno-unused-private-field
SRCS := $(wildcard *.cpp)
OBJS := $(SRCS:.cpp=.o)
all: $(OBJS)
	$(MAKE) -C user_conflict_negative all EXTRA_CXXFLAGS="$(EXTRA_CXXFLAGS)"
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(EXTRA_CXXFLAGS) -c $< -o $@
clean:
	rm -f $(OBJS)
	$(MAKE) -C user_conflict_negative clean || true
.PHONY: all clean
