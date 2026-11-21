// Host-simulation stubs for the capture-turnip-draw workflow: two optional kernel hooks drivers/Adreno740 imports weakly
// (NativeGpuTrace.h native_gpu_trace_record, NativeAdrenoUserClient.cpp native_adreno_now_us). In the kernel they come from
// NativeAdrenoPublish.cpp, which the host simulation does not build; here the trace is dropped and the clock is mach time.
#include <cstdint>
#include <mach/mach_time.h>
extern "C" void native_gpu_trace_record(uint32_t, uint32_t, const uint64_t *, uint32_t) {}
extern "C" uint64_t native_adreno_now_us(void) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom / 1000;
}
