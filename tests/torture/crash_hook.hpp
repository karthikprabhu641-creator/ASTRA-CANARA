#pragma once

#include <cstdlib>
#include <string>

namespace torture {

inline void crash_if_selected(const char* point) {
#if defined(KVDB_ENABLE_CRASH_HOOKS)
    const char* selected = std::getenv("KVDB_TEST_CRASH_POINT");
    if (selected && std::string(selected) == point) {
        std::_Exit(86);
    }
#else
    (void)point;
#endif
}

} // namespace torture
