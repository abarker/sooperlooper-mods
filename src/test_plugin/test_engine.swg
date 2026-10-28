%module test_engine
%{
#include "test_engine.hpp"
#include "test_looper.hpp"
#include "../plugin.hpp"
extern float g_late_hit_window_ms;
extern float g_late_hit_max_quant_unit_fraction;
extern float g_record_failsafe_seconds;
extern float g_mute_late_hit_ms;
extern float g_rel_sync_preroll_ms;
using namespace SooperLooper;
%}

/* Expose the late-hit window constants so tests can set them precisely. */
extern float g_late_hit_window_ms;
extern float g_late_hit_max_quant_unit_fraction;
/* Expose the record-failsafe timeout so tests can set it precisely. */
extern float g_record_failsafe_seconds;
/* Expose the mute/unmute late-hit window so tests can set it precisely. */
extern float g_mute_late_hit_ms;
/* Expose the Rel Sync extra early start (REL_SYNC_LATE_HIT). */
extern float g_rel_sync_preroll_ms;

%include "test_engine.hpp"
%include "test_looper.hpp"
%include "../plugin.hpp"
