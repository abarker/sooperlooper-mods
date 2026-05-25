%module test_engine
%{
#include "test_engine.hpp"
#include "test_looper.hpp"
#include "../plugin.hpp"
extern float g_late_hit_window_ms;
extern float g_late_hit_max_8th_fraction;
using namespace SooperLooper;
%}

/* Expose the late-hit window constants so tests can set them precisely. */
extern float g_late_hit_window_ms;
extern float g_late_hit_max_8th_fraction;

%include "test_engine.hpp"
%include "test_looper.hpp"
%include "../plugin.hpp"
