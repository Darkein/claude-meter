#pragma once
// FW_VERSION / FW_GIT are produced by tools/gen_version.py into version_gen.h
// (gitignored) before each build. The fallbacks below keep builds outside
// PlatformIO (or without git) compiling.
#if defined(__has_include)
#  if __has_include("version_gen.h")
#    include "version_gen.h"
#  endif
#endif
#ifndef FW_VERSION
#  define FW_VERSION "0.0.0"
#endif
#ifndef FW_GIT
#  define FW_GIT "nogit"
#endif
