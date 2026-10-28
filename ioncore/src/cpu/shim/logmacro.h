// Stand-in for MAME's logmacro.h: logging compiled out.
#pragma once
#ifndef LOGMASKED
#define LOGMASKED(mask, ...) do { } while (0)
#endif
#ifndef LOG
#define LOG(...) do { } while (0)
#endif
