/* Host-simulation stand-in for XNU's private <sys/resource_private.h> (xnu-12377.121.6 bsd/sys/resource_private.h), used only by
 * the capture-turnip-draw workflow: drivers/Adreno740/NativeAdrenoUserClient.cpp needs the GPU role values, and the real header
 * pulls in Apple's private availability headers, which the public SDK lacks. Values copied from that release. */
#pragma once
#define PRIO_DARWIN_GPU 5
enum {
    PRIO_DARWIN_GPU_UNKNOWN = 0x0, PRIO_DARWIN_GPU_ALLOW = 0x1, PRIO_DARWIN_GPU_DENY = 0x2, PRIO_DARWIN_GPU_BACKGROUND = 0x3,
    PRIO_DARWIN_GPU_UTILITY = 0x4, PRIO_DARWIN_GPU_UI_NON_FOCAL = 0x5, PRIO_DARWIN_GPU_UI = 0x6, PRIO_DARWIN_GPU_UI_FOCAL = 0x7,
};
