// #pragma once
// #include <stdexcept>
// #include <string>

// #include "GeniePipeline.h"
// #include "GenieNode.h"
// #include "GenieCommon.h"
// #include "GenieDialog.h"
// #include "GenieSampler.h"


#pragma once
#include <stdexcept>
#include <string>

#ifndef GENIE_STATUS_SUCCESS
#define GENIE_STATUS_SUCCESS 0
#endif

#define GENIE_CHECK(stmt) do {                                      \
    auto _rc = (stmt);                                              \
    if (_rc != GENIE_STATUS_SUCCESS) {                              \
        throw std::runtime_error(                                   \
            std::string("GENIE call failed: ") + #stmt +            \
            " status=" + std::to_string((int)_rc));                 \
    }                                                               \
} while(0)