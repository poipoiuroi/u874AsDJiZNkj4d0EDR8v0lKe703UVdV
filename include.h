#pragma once
#ifndef _INCLUDE_H_
#define _INCLUDE_H_

#include <fstream>
#include <memory>
#include <mutex>
#include <queue>
#include <chrono>
#include <thread>
#include <string>
#include <array>
#include <vector>
#include <windows.h>

#include <SDL3/SDL.h>
#include <fdk-aac/aacdecoder_lib.h>
#include <libde265/de265.h>

#include "utils.h"
#include "cryptor.h"
#include "memstream.h"
#include "player.h"

#pragma comment(lib, "setupapi")
#pragma comment(lib, "imm32")
#pragma comment(lib, "version")
#pragma comment(lib, "winmm")

#pragma comment(lib, "fdk-aac")
#pragma comment(lib, "libde265")
#pragma comment(lib, "SDL3-static")

#endif