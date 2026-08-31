%module test_engine
%{
#include "test_engine.hpp"
#include "test_looper.hpp"
#include "../plugin.hpp"
extern float g_late_hit_window_ms;
extern float g_late_hit_max_quant_unit_fraction;
extern float g_record_failsafe_seconds;
using namespace SooperLooper;
%}

/* Expose the late-hit window constants so tests can set them precisely. */
extern float g_late_hit_window_ms;
extern float g_late_hit_max_quant_unit_fraction;
/* Expose the record-failsafe timeout so tests can set it precisely. */
extern float g_record_failsafe_seconds;

%include "test_engine.hpp"
%include "test_looper.hpp"
%include "../plugin.hpp"
