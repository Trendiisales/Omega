#pragma once

#include <string>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <set>
#include <chrono>
#include <thread>
#include <atomic>

#include "SymbolSupervisor.hpp"
#include "logging.hpp"
#include "order_exec.hpp"
#include "OmegaTelemetryWriter.hpp"
#include "OmegaPartialExit.hpp"
#include "OmegaVolTargeter.hpp"
#include "sizing.hpp"

/*
  IMPORTANT:
  This file intentionally contains function implementations used by the
  engine headers. Because it is header-only we must ensure it is included
  only once per translation unit. The pragma above guarantees that.
*/

/* --- keep the rest of the original file content below this line --- */

