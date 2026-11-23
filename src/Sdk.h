#pragma once
// Simulator-only portability shim. Vendor headers and real Windows builds are unchanged.
#if defined(CONNECT_LAB_SIMULATED) && !defined(_WIN32)
#define __declspec(value)
#ifndef __cdecl
#define __cdecl
#endif
#endif
#include <viaim_connect/ViaimConnect.h>
