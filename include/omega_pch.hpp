// ==============================================================================
// omega_pch.hpp -- Precompiled Header for Omega
//
// Contains all stable, rarely-changing includes:
//   - Windows / WinSock2
//   - OpenSSL
//   - C++ STL
//
// DOES NOT include Omega .hpp headers -- those change frequently and must
// remain outside the PCH so edits to engine headers don't invalidate it.
//
// Build time impact: ~40-60% faster recompile on MSVC /O2 after first build.
// The PCH is only rebuilt when THIS file changes -- not when engine headers change.
// ==============================================================================
#pragma once

// Windows / networking -- must be first
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mstcpip.h>

// OpenSSL
#include <openssl/ssl.h>
#include <openssl/err.h>

// C++ standard library -- all stable, never change
#include <iostream>
#include <atomic>
#include <string>
#include <string_view>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <direct.h>
#include <chrono>
#include <memory>
#include <mutex>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <deque>
#include <cmath>
#include <csignal>
#include <functional>
#include <cstdint>
#include <cstring>
#include <thread>
#include <condition_variable>
#include <optional>
#include <variant>
#include <array>
#include <numeric>
