VIGEM_ROOT = ViGEmClient
CPPFLAGS =  -O2 -I$(VIGEM_ROOT)/include
CXXFLAGS = -std=c++20 -Wall -Wextra -Werror
LDFLAGS = -s -static -lxinput -lsetupapi
TARGET = gamepad-slotter.exe

default: $(TARGET)

$(TARGET): main.o vigemclient.o
	$(CXX) -o $@ $^ $(LDFLAGS)

# Don't bother with depency on `.h` files; they won't change

main.o: main.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

# Rebuild ViGEmClient manually, it's simpler than using cmake
vigemclient.o: $(VIGEM_ROOT)/src/ViGEmClient.cpp
	$(CXX) $(CPPFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) *.o

.PHONY: clean
