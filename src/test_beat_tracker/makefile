# Unit tests for ../beat_tracker.hpp (ROADMAP entry 15).  No JACK needed.
test_beat_tracker: test_beat_tracker.cpp ../beat_tracker.hpp
	g++ -std=c++11 -O2 -Wall -Wextra -o $@ test_beat_tracker.cpp

check: test_beat_tracker test_beat_tracker_reanchor
	./test_beat_tracker
	./test_beat_tracker_reanchor

# The same tests with ALB_CYCLE_CHANGE_REANCHOR on (engine.hpp; off by
# default), which adds the cycle-origin tests (ROADMAP entry 21).
test_beat_tracker_reanchor: test_beat_tracker.cpp ../beat_tracker.hpp
	g++ -std=c++11 -O2 -Wall -Wextra -DALB_CYCLE_CHANGE_REANCHOR -o $@ test_beat_tracker.cpp

clean:
	rm -f test_beat_tracker test_beat_tracker_reanchor
