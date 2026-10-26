#include "test_looper.hpp"
#include <jack/jack.h>

class TestEngine 
{
    public:
        //------ albBegin ----------------------------------
        // Was: TestEngine(const char* name), always one channel.
        TestEngine(const char* name, int chans = 1);
        //------ albEnd ----------------------------------
        ~TestEngine();

        jack_client_t* jack;
        SooperLooper::TestLooper * looper;

        int get_sample_rate() const { return jack_get_sample_rate(jack); }
    private:
        int connect_to_jack(const char* name);
        static int _process_callback (jack_nframes_t, void*);
        int process_callback (jack_nframes_t);
};

