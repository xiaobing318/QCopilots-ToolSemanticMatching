// SPDX-License-Identifier: MIT
// Adapted from QCopilots-Search (MIT).
#pragma once

#ifdef _WIN32
#include <clocale>
#include <string>
#endif

namespace qctsm {

// Upstream narrow-path APIs use the shared UCRT locale on Windows. Restrict
// UTF-8 to the calling thread and LC_CTYPE so numeric parsing is unaffected.
class ScopedUtf8Locale final {
public:
    ScopedUtf8Locale()
    {
#ifdef _WIN32
        const char *current = std::setlocale(LC_CTYPE, nullptr);
        if (!current) return;
        previousLocale_ = current;
        previousMode_ = _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
        if (previousMode_ == -1) return;
        modeChanged_ = true;
        valid_ = std::setlocale(LC_CTYPE, ".UTF8") != nullptr;
#endif
    }

    ~ScopedUtf8Locale()
    {
#ifdef _WIN32
        if (modeChanged_) {
            std::setlocale(LC_CTYPE, previousLocale_.c_str());
            _configthreadlocale(previousMode_);
        }
#endif
    }

    ScopedUtf8Locale(const ScopedUtf8Locale &) = delete;
    ScopedUtf8Locale &operator=(const ScopedUtf8Locale &) = delete;
    bool isValid() const noexcept { return valid_; }

private:
#ifdef _WIN32
    std::string previousLocale_;
    int previousMode_ = -1;
    bool modeChanged_ = false;
    bool valid_ = false;
#else
    bool valid_ = true;
#endif
};

} // namespace qctsm
