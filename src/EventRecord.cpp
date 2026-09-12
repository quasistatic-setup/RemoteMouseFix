#include "rmf/EventRecord.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

namespace rmf {
namespace {

// Fixed widths keep the columns aligned so a human can scan a burst of events and see
// at a glance where a delta explodes.
void AppendFormatted(std::wstring& out, const wchar_t* format, ...) {
    wchar_t buffer[512];
    va_list args;
    va_start(args, format);
    const int written = vswprintf(buffer, 512, format, args);
    va_end(args);
    if (written > 0) {
        out.append(buffer, static_cast<std::size_t>(written));
    }
}

} // namespace

const wchar_t* EventKindName(EventKind k) {
    switch (k) {
        case EventKind::Move:         return L"MOVE ";
        case EventKind::LButtonDown:  return L"LDOWN";
        case EventKind::LButtonUp:    return L"LUP  ";
        case EventKind::RButtonDown:  return L"RDOWN";
        case EventKind::RButtonUp:    return L"RUP  ";
        case EventKind::MButtonDown:  return L"MDOWN";
        case EventKind::MButtonUp:    return L"MUP  ";
        case EventKind::XButtonDown:  return L"XDOWN";
        case EventKind::XButtonUp:    return L"XUP  ";
        case EventKind::Wheel:        return L"WHEEL";
        case EventKind::HWheel:       return L"HWHEL";
        case EventKind::Apply:        return L"APPLY";
        default:                      return L"?????";
    }
}

bool IsButtonEvent(EventKind k) {
    switch (k) {
        case EventKind::LButtonDown: case EventKind::LButtonUp:
        case EventKind::RButtonDown: case EventKind::RButtonUp:
        case EventKind::MButtonDown: case EventKind::MButtonUp:
        case EventKind::XButtonDown: case EventKind::XButtonUp:
            return true;
        default:
            return false;
    }
}

// Wall clock derived from the QPC offset against the session start, so the millisecond
// field is meaningful rather than quantised to the clock tick, followed by the elapsed
// seconds column.
static void AppendTimestamp(std::wstring& line, std::int64_t qpcTicks, std::int64_t qpcFreq,
                     std::int64_t qpcStart, const FILETIME& wallStart) {
    const std::int64_t elapsedTicks = qpcTicks - qpcStart;
    const double elapsedSec = (qpcFreq > 0)
        ? static_cast<double>(elapsedTicks) / static_cast<double>(qpcFreq)
        : 0.0;

    ULARGE_INTEGER now {};
    now.LowPart  = wallStart.dwLowDateTime;
    now.HighPart = wallStart.dwHighDateTime;
    now.QuadPart += static_cast<ULONGLONG>(elapsedSec * 10'000'000.0);

    FILETIME nowFt {};
    nowFt.dwLowDateTime  = now.LowPart;
    nowFt.dwHighDateTime = now.HighPart;

    // wallStart is UTC; the header and footer print local time, so convert here.
    FILETIME localFt {};
    if (!FileTimeToLocalFileTime(&nowFt, &localFt)) {
        localFt = nowFt;
    }
    SYSTEMTIME st {};
    FileTimeToSystemTime(&localFt, &st);

    AppendFormatted(line, L"%02u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    AppendFormatted(line, L" %10.3f", elapsedSec);
}

std::wstring FormatEventLine(const DecoratedEvent& ev, std::int64_t qpcFreq,
                             std::int64_t qpcStart, const FILETIME& wallStart) {
    const RawEvent& raw = *ev.raw;

    std::wstring line;
    line.reserve(260);
    AppendTimestamp(line, raw.qpcTicks, qpcFreq, qpcStart, wallStart);
    AppendFormatted(line, L" #%09llu", static_cast<unsigned long long>(raw.seq));
    AppendFormatted(line, L" %ls", EventKindName(raw.kind));

    AppendFormatted(line, L" scr=(%+06ld,%+06ld)", raw.pt.x, raw.pt.y);
    AppendFormatted(line, L" d=(%+05ld,%+05ld)", ev.dx, ev.dy);

    if (ev.dtMs >= 0) {
        AppendFormatted(line, L" dt=%4ldms", ev.dtMs);
    } else {
        line += L" dt=   -ms";
    }

    if (ev.haveClient) {
        AppendFormatted(line, L" cli=(%+06ld,%+06ld)", ev.client.x, ev.client.y);
        AppendFormatted(line, L" ctr=(%+06ld,%+06ld)", ev.centreDx, ev.centreDy);
    } else {
        line += L" cli=(     -,     -) ctr=(     -,     -)";
    }

    if (ev.haveAnchor) {
        AppendFormatted(line, L" anc=(%+06ld,%+06ld)", ev.anchorDx, ev.anchorDy);
    } else {
        line += L" anc=(     -,     -)";
    }

    // Injection flags are the crux of the remote-client question: anything a remote client
    // synthesises carries LLMHF_INJECTED, and LOWER_IL_INJECTED additionally says it came
    // from a lower integrity level.
    wchar_t flags[3] = {L'-', L'-', L'\0'};
    if (ev.injected)        flags[0] = L'I';
    if (ev.lowerIlInjected) flags[1] = L'L';
    AppendFormatted(line, L" flg=%ls", flags);
    AppendFormatted(line, L" cur=%lc", raw.cursorHidden ? L'H' : L'-');

    AppendFormatted(line, L" xi=0x%016llx", static_cast<unsigned long long>(raw.extraInfo));

    if (raw.kind == EventKind::Wheel || raw.kind == EventKind::HWheel) {
        AppendFormatted(line, L" wheel=%+5d", GET_WHEEL_DELTA_WPARAM(raw.mouseData));
    } else if (raw.kind == EventKind::XButtonDown || raw.kind == EventKind::XButtonUp) {
        AppendFormatted(line, L" xbtn=%u", HIWORD(raw.mouseData));
    }

    if (raw.suppressed) {
        AppendFormatted(line, L" DROP rd=(%+ld,%+ld) via=%ls",
                        raw.remoteDx, raw.remoteDy, CorrectionModeName(raw.correctionMode));
        if (raw.trackingReset) {
            line += L" reset";
        }
    }
    if (raw.ownInput)     line += L" OWN";
    if (ev.atAnchor)      line += L" ANCHOR";
    if (ev.looksLikeJump) line += L" JUMP";

    AppendFormatted(line, L" pid=%-6lu %ls", raw.foregroundPid, ev.processName.c_str());

    return line;
}

} // namespace rmf

namespace rmf {

std::wstring FormatApplyLine(const RawEvent& raw, std::int64_t qpcFreq,
                             std::int64_t qpcStart, const FILETIME& wallStart) {
    std::wstring line;
    line.reserve(200);
    AppendTimestamp(line, raw.qpcTicks, qpcFreq, qpcStart, wallStart);
    // `after` is read right after SendInput returned, which usually precedes the input
    // thread processing the event; the matching OWN move line shows where it really landed.
    AppendFormatted(line, L" #--------- APPLY via=%ls rd=(%+ld,%+ld) before=(%ld,%ld) target=(%ld,%ld) after=(%ld,%ld)",
                    CorrectionModeName(raw.correctionMode), raw.remoteDx, raw.remoteDy,
                    raw.applyBefore.x, raw.applyBefore.y, raw.applyTarget.x, raw.applyTarget.y,
                    raw.pt.x, raw.pt.y);
    if (raw.applyOk) {
        line += L" ok";
    } else {
        AppendFormatted(line, L" FAILED error=%lu", raw.applyError);
    }
    // Absolute QPC milliseconds: the same clock on every process of the machine, so apply
    // lines can be aligned exactly with another process's trace (MouseLookProbe).
    AppendFormatted(line, L" qpc_ms=%lld",
                    static_cast<long long>(qpcFreq > 0 ? raw.qpcTicks * 1000 / qpcFreq : 0));
    return line;
}

} // namespace rmf
