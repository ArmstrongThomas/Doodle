#ifndef DOODLE_TICKET_FLOW_H
#define DOODLE_TICKET_FLOW_H

#include <stddef.h>
#include <stdio.h>

namespace Doodle
{

inline void buildUserReportDraft(char *subject, size_t subjectSize,
                                 char *details, size_t detailsSize,
                                 const char *displayName,
                                 const char *username,
                                 const char *identityOrSession,
                                 const char *channel,
                                 const char *reason)
{
    const char *safeName =
        displayName && displayName[0] ? displayName : "Anonymous";
    const char *safeUsername =
        username && username[0] ? username : "anonymous";
    const char *safeIdentity =
        identityOrSession && identityOrSession[0]
            ? identityOrSession : "unknown";
    const char *safeChannel =
        channel && channel[0] ? channel : "main";
    const char *safeReason =
        reason && reason[0] ? reason : "No reason provided";

    if (subject && subjectSize > 0)
        snprintf(subject, subjectSize, "Report: %.48s", safeName);
    if (details && detailsSize > 0)
    {
        snprintf(details, detailsSize,
                 "User: %.24s (%.24s)\n"
                 "Identity: %.39s\n"
                 "Channel: %.24s\n"
                 "Reason: %.90s",
                 safeName, safeUsername, safeIdentity,
                 safeChannel, safeReason);
    }
}

} // namespace Doodle

#endif
