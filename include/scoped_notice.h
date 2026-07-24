#ifndef DOODLE_SCOPED_NOTICE_H
#define DOODLE_SCOPED_NOTICE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace Doodle
{

template <size_t Capacity>
class ScopedNotice
{
public:
    ScopedNotice()
        : scope_(-1), expiresAt_(0), pending_(false)
    {
        text_[0] = '\0';
    }

    void set(const char *text, int scope, uint64_t nowMs,
             uint64_t durationMs, bool pending = false)
    {
        snprintf(text_, sizeof(text_), "%s", text ? text : "");
        scope_ = scope;
        expiresAt_ = text_[0] ? nowMs + durationMs : 0;
        pending_ = text_[0] && pending;
    }

    void clear()
    {
        text_[0] = '\0';
        scope_ = -1;
        expiresAt_ = 0;
        pending_ = false;
    }

    bool expire(uint64_t nowMs)
    {
        if (!text_[0] || !expiresAt_ || nowMs < expiresAt_)
            return false;
        clear();
        return true;
    }

    bool visible(int scope, uint64_t nowMs) const
    {
        return text_[0] && scope_ == scope &&
               expiresAt_ > 0 && nowMs < expiresAt_;
    }

    const char *text() const
    {
        return text_;
    }

    bool pending() const
    {
        return pending_;
    }

    bool clearPending()
    {
        if (!pending_)
            return false;
        clear();
        return true;
    }

private:
    char text_[Capacity];
    int scope_;
    uint64_t expiresAt_;
    bool pending_;
};

} // namespace Doodle

#endif
