#pragma once

#include <cstdint>

enum InputLatencyTraceStage : uint32_t
{
	INPUT_LATENCY_T2_BACKEND = 2,
	INPUT_LATENCY_T3_WAYLOCK = 3,
	INPUT_LATENCY_T4_NOTIFY = 4,
	INPUT_LATENCY_T5_FLUSH = 5,
	INPUT_LATENCY_D0_POLL_WAKE = 14,
	INPUT_LATENCY_D1_READ_DONE = 15,
	INPUT_LATENCY_D2_DISPATCH = 16,
};

void input_latency_trace_init();
void input_latency_trace_shutdown();
void input_latency_trace_record_input_cycle( InputLatencyTraceStage stage );
void input_latency_trace_record_key( InputLatencyTraceStage stage,
	uint32_t key, bool pressed );
