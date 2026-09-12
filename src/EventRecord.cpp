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

std::wstring FormatEventLine(const DecoratedEvent& ev, std::int64_t qpcFreq,
                             std::int64_t qpcStart, const FILETIME& wallStart) {
    const RawEvent& raw = *ev.raw;

    // Wall clock: derived from the QPC offset against the session start, so the
    // millisecond field is actually meaningful rather than quantised to the clock tick.
    const std::int64_t elapsedTicks = raw.qpcTicks - qpcStart;
    const double elapsedSec = (qpcFreq > 0)
        ? static_cast<double>(elapsedTicks) / static_cast<double>(qpcFreq)
        : 0.0;

    ULARGE_INTEGER start {};
    start.LowPart  = wallStart.dwLowDateTime;
    start.HighPart = wallStart.dwHighDateTime;
    ULARGE_INTEGER now = start;
    now.QuadPart += static_cast<ULONGLONG>(elapsedSec * 10'000'000.0);

    FILETIME nowFt {};
    nowFt.dwLowDateTime  = now.LowPart;
    nowFt.dwHighDateTime = now.HighPart;

    // wallStart comes from GetSystemTimeAsFileTime and is therefore UTC, while the
    // session header and footer print local time. Convert here so every timestamp in
    // the file is in the same zone as the operator's clock.
    FILETIME localFt {};
    if (!FileTimeToLocalFileTime(&nowFt, &localFt)) {
        localFt = nowFt;
    }
    SYSTEMTIME st {};
    FileTimeToSystemTime(&localFt, &st);

    std::wstring line;
    line.reserve(220);

    AppendFormatted(line, L"%02u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    AppendFormatted(line, L" %10.3f", elapsedSec);
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

    // Injection flags are the crux of the TeamViewer question: anything TeamViewer
    // synthesises should carry LLMHF_INJECTED, and LOWER_IL_INJECTED additionally says
    // it came from a lower integrity level.
    wchar_t flags[3] = {L'-', L'-', L'\0'};
    if (ev.injected)         flags[0] = L'I';
    if (ev.lowerIlInjected)  flags[1] = L'L';
    AppendFormatted(line, L" flg=%ls", flags);

    AppendFormatted(line, L" xi=0x%016llx", static_cast<unsigned long long>(raw.extraInfo));

    if (raw.kind == EventKind::Wheel || raw.kind == EventKind::HWheel) {
        AppendFormatted(line, L" wheel=%+5d", GET_WHEEL_DELTA_WPARAM(raw.mouseData));
    } else if (raw.kind == EventKind::XButtonDown || raw.kind == EventKind::XButtonUp) {
        AppendFormatted(line, L" xbtn=%u", HIWORD(raw.mouseData));
    }

    if (ev.looksLikeRecenter) line += L" RECENTER";
    if (ev.looksLikeJump)     line += L" JUMP";

    AppendFormatted(line, L" pid=%-6lu %ls", raw.foregroundPid, ev.processName.c_str());

    return line;
}

} // namespace rmf
