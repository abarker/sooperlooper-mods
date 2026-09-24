# Unit tests for ../beat_tracker.hpp (ROADMAP entry 15).  No JACK needed.
test_beat_tracker: test_beat_tracker.cpp ../beat_tracker.hpp
	g++ -std=c++11 -O2 -Wall -Wextra -o $@ test_beat_tracker.cpp

check: test_beat_tracker
	./test_beat_tracker

clean:
	rm -f test_beat_tracker
