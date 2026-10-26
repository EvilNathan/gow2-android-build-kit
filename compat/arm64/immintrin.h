// Android arm64 compatibility header.
// PS2Recomp already supports ARM through sse2neon (USE_SSE2NEON), but a few
// translation units still include the x86 header names unconditionally.
// This directory is added to the include path for arm64 builds only.
#pragma once
#include "sse2neon.h"
