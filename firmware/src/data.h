#pragma once
#include <Arduino.h>

// Live Claude Code activity (cs field). WAITING means at least one tool-permission
// prompt is queued for remote approval.
enum claude_state_t {
    CLAUDE_IDLE = 0,      // a session finished its turn, awaiting the next message
    CLAUDE_WORKING = 1,   // a session is actively running
    CLAUDE_WAITING = 2,   // a tool-permission prompt is pending (queued)
    CLAUDE_NONE = 3,      // no recent session activity
    CLAUDE_QUESTION = 4,  // blocked on a user question (AskUserQuestion / elicitation)
};

struct UsageData {
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    char status[16];         // "allowed" or "limited"
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse

    // Live Claude Code state (set from the daemon's hook-derived fields).
    claude_state_t claude_state;  // cs — idle/working/waiting
    int  approval_count;          // aq — pending approvals queued (0 = none)
    char approval_sid[40];        // as — front-of-queue session id (decision target)
    char pending_tool[16];        // tn — front-of-queue tool name
    char pending_detail[64];      // td — front-of-queue tool detail (cmd/path/url)
};
