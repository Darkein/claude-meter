#pragma once
#include <Arduino.h>

// ---- BLE wire protocol (daemon -> device, JSON written to the RX char) -------
// The daemon merges usage + live-state into one compact JSON object. Keys are
// short to fit the BLE MTU; unknown keys are ignored, missing keys keep their
// defaults (so adding a field is backward-compatible). Fields:
//   s  float  5h-window utilization %        -> session_pct
//   sr int    minutes until the 5h reset     -> session_reset_mins
//   w  float  7d-window utilization %        -> weekly_pct
//   wr int    minutes until the 7d reset     -> weekly_reset_mins
//   st str    "allowed" / "limited"          -> status
//   ok bool   poll succeeded                 -> ok
//   ec str    error code (auth/network/...)  -> (not stored; ok+em drive the UI)
//   em str    device-displayable error msg   -> error_msg  (shown when ok=false)
//   t  uint   local wall-clock epoch (sec)   -> host_epoch  (0/absent = no clock)
//   cs int    claude_state (0..4, see below) -> claude_state
//   aq int    approval queue length (total)  -> approval_count
//   q  array  pending approvals (bounded):   -> approvals[] / approval_n
//             [{tn:tool, td:detail}, ...] FIFO order, so the device can swipe
//             between each. aq may exceed the array length (badge "i / aq").
// (The daemon's internal `_queue` field is stripped before sending.)
// `t` carries LOCAL wall time as an epoch (the device has no timezone); store
// and display it verbatim.
// ------------------------------------------------------------------------------

// Live Claude Code activity (cs field). WAITING means at least one tool-permission
// prompt is pending on the host (shown on the device for awareness; the native
// Claude Code prompt is the sole approver).
enum claude_state_t {
    CLAUDE_IDLE = 0,      // a session finished its turn, awaiting the next message
    CLAUDE_WORKING = 1,   // a session is actively running
    CLAUDE_WAITING = 2,   // a tool-permission prompt is pending (queued)
    CLAUDE_NONE = 3,      // no recent session activity
    CLAUDE_QUESTION = 4,  // blocked on a user question (AskUserQuestion / elicitation)
};

// One pending tool-permission prompt (a slot in the approval queue). The device
// can swipe through these; the daemon sends up to MAX_APPROVALS of them. Bounded
// to keep the worst-case BLE JSON under the 512-byte RX buffer.
#define MAX_APPROVALS 4
struct Approval {
    char tool[16];    // tool name (e.g. "Bash", "Write")
    char detail[64];  // command / path / url, truncated by the daemon
};

struct UsageData {
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    char status[16];         // "allowed" or "limited"
    bool ok;                 // poll succeeded (false => error_msg is set)
    char error_msg[96];      // em — device-displayable error (empty when ok)
    bool valid;              // false until first successful parse

    uint32_t host_epoch;     // t — local wall-clock epoch from the host (0 = absent)

    // Live Claude Code state (set from the daemon's hook-derived fields).
    claude_state_t claude_state;          // cs — idle/working/waiting
    int      approval_count;              // aq — true total pending (may exceed approval_n)
    Approval approvals[MAX_APPROVALS];    // q — bounded detail list, FIFO order
    uint8_t  approval_n;                  // number of entries actually filled in approvals[]
};
