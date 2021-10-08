PROGRAMS = screen-worms-server screen-worms-client
CXX = g++
CXXFLAGS = -Wall -Wextra -O2 -std=c++17

all: $(PROGRAMS)

err.o: err.cpp err.h
	$(CXX) -c $(CXXFLAGS) -o $@ $<
    
crc.o: crc.cpp crc.h
	$(CXX) -c $(CXXFLAGS) -o $@ $<

screen-worms-client.o: worms-client.cpp communication.h crc.h
	$(CXX) -c $(CXXFLAGS) -o $@ $<
	
screen-worms-server.o: worms-server.cpp communication.h crc.h
	$(CXX) -c $(CXXFLAGS) -o $@ $<

screen-worms-client: screen-worms-client.o crc.o err.o
	$(CXX) -pthread -o $@ $^
	
screen-worms-server: screen-worms-server.o crc.o err.o
	$(CXX) -pthread -o $@ $^


.PHONY: all clean

clean:
	rm -rf $(PROGRAMS) *.o
