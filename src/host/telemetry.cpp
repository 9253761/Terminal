/********************************************************
*                                                       *
*   Copyright (C) Microsoft. All rights reserved.       *
*                                                       *
********************************************************/

#include "precomp.h"
#include <Intsafe.h>
#include "psapi.h"
#include "Shlwapi.h"
#include "telemetry.hpp"
#include <time.h>

TRACELOGGING_DEFINE_PROVIDER(g_hConhostV2EventTraceProvider,
    "Microsoft.Windows.Console.Host",
    // {fe1ff234-1f09-50a8-d38d-c44fab43e818}
    (0xfe1ff234, 0x1f09, 0x50a8, 0xd3, 0x8d, 0xc4, 0x4f, 0xab, 0x43, 0xe8, 0x18),
    TraceLoggingOptionMicrosoftTelemetry());
#pragma warning(push)
// Disable 4351 so we can initialize the arrays to 0 without a warning.
#pragma warning(disable:4351)
Telemetry::Telemetry()
    : _fpFindStringLengthAverage(0),
    _fpDirectionDownAverage(0),
    _fpMatchCaseAverage(0),
    _uiFindNextClickedTotal(0),
    _tStartedAt(0),
    _wchProcessFileNames(),
    // Start at position 1, since the first 2 bytes contain the number of strings.
    _iProcessFileNamesNext(1),
    _iProcessConnectedCurrently(SIZE_MAX),
    _rgiProccessFileNameIndex(),
    _rguiProcessFileNamesCount(),
    _rgiAlphabeticalIndex(),
    _rguiProcessFileNamesCodesCount(),
    _rguiProcessFileNamesFailedCodesCount(),
    _rguiProcessFileNamesFailedOutsideCodesCount(),
    _rguiTimesApiUsed(),
    _rguiTimesApiUsedAnsi(),
    _uiNumberProcessFileNames(0),
    _fBashUsed(false),
    _fKeyboardTextEditingUsed(false),
    _fKeyboardTextSelectionUsed(false),
    _fUserInteractiveForTelemetry(false),
    _fCtrlPgUpPgDnUsed(false)
{
    time(&_tStartedAt);
    TraceLoggingRegister(g_hConhostV2EventTraceProvider);
    TraceLoggingWriteStart(_activity, "ActivityStart");
    // initialize wil tracelogging
    wil::SetResultLoggingCallback(&Tracing::TraceFailure);
    wil::g_pfnShouldOutputDebugString = [] { return !!IsDebuggerPresent(); };
}
#pragma warning(pop)

Telemetry::~Telemetry()
{
    TraceLoggingWriteStop(_activity, "ActivityStop");
    TraceLoggingUnregister(g_hConhostV2EventTraceProvider);
}

void Telemetry::SetUserInteractive()
{
    _fUserInteractiveForTelemetry = true;
}

void Telemetry::SetCtrlPgUpPgDnUsed()
{
    _fCtrlPgUpPgDnUsed = true;
    SetUserInteractive();
}

void Telemetry::SetWindowSizeChanged()
{
    SetUserInteractive();
}

void Telemetry::SetContextMenuUsed()
{
    SetUserInteractive();
}

void Telemetry::SetKeyboardTextSelectionUsed()
{
    _fKeyboardTextSelectionUsed = true;
    SetUserInteractive();
}

void Telemetry::SetKeyboardTextEditingUsed()
{
    _fKeyboardTextEditingUsed = true;
    SetUserInteractive();
}

// Log an API call was used.
void Telemetry::LogApiCall(_In_ ApiCall const api, _In_ BOOLEAN const fUnicode)
{
    // Initially we thought about passing over a string (ex. "XYZ") and use a dictionary data type to hold the counts.
    // However we would have to search through the dictionary every time we called this method, so we decided
    // to use an array which has very quick access times.
    // The downside is we have to create an enum type, and then convert them to strings when we finally
    // send out the telemetry, but the upside is we should have very good performance.
    if (fUnicode)
    {
        _rguiTimesApiUsed[api]++;
    }
    else
    {
        _rguiTimesApiUsedAnsi[api]++;
    }
}

// Log an API call was used.
void Telemetry::LogApiCall(_In_ ApiCall const api)
{
    _rguiTimesApiUsed[api]++;
}

// Log usage of the Find Dialog.
void Telemetry::LogFindDialogNextClicked(_In_ const unsigned int uiStringLength, _In_ const bool fDirectionDown, _In_ const bool fMatchCase)
{
    // Don't send telemetry for every time it's used, as this will help reduce the load on our servers.
    // Instead just create a running average of the string length, the direction down radio
    // button, and match case checkbox.
    _fpFindStringLengthAverage = ((_fpFindStringLengthAverage * _uiFindNextClickedTotal + uiStringLength) / (_uiFindNextClickedTotal + 1));
    _fpDirectionDownAverage = ((_fpDirectionDownAverage * _uiFindNextClickedTotal + (fDirectionDown ? 1 : 0)) / (_uiFindNextClickedTotal + 1));
    _fpMatchCaseAverage = ((_fpMatchCaseAverage * _uiFindNextClickedTotal + (fMatchCase ? 1 : 0)) / (_uiFindNextClickedTotal + 1));
    _uiFindNextClickedTotal++;
}

// Find dialog was closed, now send out the telemetry.
void Telemetry::FindDialogClosed()
{
#pragma prefast(suppress:__WARNING_NONCONST_LOCAL, "Activity can't be const, since it's set to a random value on startup.")
    TraceLoggingWriteTagged(_activity,
        "FindDialogUsed",
        TraceLoggingValue(_fpFindStringLengthAverage, "StringLengthAverage"),
        TraceLoggingValue(_fpDirectionDownAverage, "DirectionDownAverage"),
        TraceLoggingValue(_fpMatchCaseAverage, "MatchCaseAverage"),
        TraceLoggingValue(_uiFindNextClickedTotal, "FindNextButtonClickedTotal"),
        TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES));

    // Get ready for the next time the dialog is used.
    _fpFindStringLengthAverage = 0;
    _fpDirectionDownAverage = 0;
    _fpMatchCaseAverage = 0;
    _uiFindNextClickedTotal = 0;
}

// Total up all the used VT100 codes and assign them to the last process that was attached.
// We originally did this when each process disconnected, but some processes don't
// disconnect when the conhost process exits.  So we have to remember the last process that connected.
void Telemetry::TotalCodesForPreviousProcess()
{
    // Get the values even if we aren't recording the previously connected process, since we want to reset them to 0.
    unsigned int _uiTimesUsedCurrent = TermTelemetry::Instance().GetAndResetTimesUsedCurrent();
    unsigned int _uiTimesFailedCurrent = TermTelemetry::Instance().GetAndResetTimesFailedCurrent();
    unsigned int _uiTimesFailedOutsideRangeCurrent = TermTelemetry::Instance().GetAndResetTimesFailedOutsideRangeCurrent();

    if (_iProcessConnectedCurrently < c_iMaxProcessesConnected)
    {
        _rguiProcessFileNamesCodesCount[_iProcessConnectedCurrently] += _uiTimesUsedCurrent;
        _rguiProcessFileNamesFailedCodesCount[_iProcessConnectedCurrently] += _uiTimesFailedCurrent;
        _rguiProcessFileNamesFailedOutsideCodesCount[_iProcessConnectedCurrently] += _uiTimesFailedOutsideRangeCurrent;

        // Don't total any more process connected telemetry, unless a new processes attaches that we want to gather.
        _iProcessConnectedCurrently = SIZE_MAX;
    }
}

// Tries to find the process name amongst our previous process names by doing a binary search.
// The main difference between this and the standard bsearch library call, is that if this
// can't find the string, it returns the position the new string should be inserted at.  This saves
// us from having an additional search through the array, and improves performance.
bool Telemetry::FindProcessName(_In_ const WCHAR* pszProcessName, _Out_ size_t *iPosition) const
{
    int iMin = 0;
    int iMid = 0;
    int iMax = _uiNumberProcessFileNames - 1;
    int result = 0;

    while (iMin <= iMax)
    {
        iMid = (iMax + iMin) / 2;
        // Use a case-insensitive comparison.  We do support running Linux binaries now, but we haven't seen them connect
        // as processes, and even if they did, we don't care about the difference in running emacs vs. Emacs.
        result = _wcsnicmp(pszProcessName, _wchProcessFileNames + _rgiProccessFileNameIndex[_rgiAlphabeticalIndex[iMid]], MAX_PATH);
        if (result < 0)
        {
            iMax = iMid - 1;
        }
        else if (result > 0)
        {
            iMin = iMid + 1;
        }
        else
        {
            // Found the string.
            *iPosition = iMid;
            return true;
        }
    }

    // Let them know which position to insert the string at.
    *iPosition = (result > 0) ? iMid + 1 : iMid;
    return false;
}

// Log a process name and number of times it has connected to the console in preparation to send through telemetry.
// We were considering sending out a log of telemetry when each process connects, but then the telemetry can get
// complicated and spammy, especially since command line utilities like help.exe and where.exe are considered processes.
// Don't send telemetry for every time a process connects, as this will help reduce the load on our servers.
// Just save the name and count, and send the telemetry before the console exits.
void Telemetry::LogProcessConnected(_In_ const HANDLE hProcess)
{
    // This is a bit of processing, so don't do it for the 95% of machines that aren't being sampled.
    if (TraceLoggingProviderEnabled(g_hConhostV2EventTraceProvider, 0, MICROSOFT_KEYWORD_MEASURES))
    {
        TotalCodesForPreviousProcess();

        // Don't initialize wszFilePathAndName, QueryFullProcessImageName does that for us.  Use QueryFullProcessImageName instead of
        // GetProcessImageFileName because we need the path to begin with a drive letter and not a device name.
        WCHAR wszFilePathAndName[MAX_PATH];
        DWORD dwSize = ARRAYSIZE(wszFilePathAndName);
        if (QueryFullProcessImageName(hProcess, 0, wszFilePathAndName, &dwSize))
        {
            // Stripping out the path also helps with PII issues in case they launched the program
            // from a path containing their username.
            PWSTR pwszFileName = PathFindFileName(wszFilePathAndName);

            size_t iFileName;
            if (FindProcessName(pwszFileName, &iFileName))
            {
                // We already logged this process name, so just increment the count.
                _iProcessConnectedCurrently = _rgiAlphabeticalIndex[iFileName];
                _rguiProcessFileNamesCount[_iProcessConnectedCurrently]++;
            }
            else if ((_uiNumberProcessFileNames < ARRAYSIZE(_rguiProcessFileNamesCount)) &&
                (_iProcessFileNamesNext < ARRAYSIZE(_wchProcessFileNames) - 10))
            {
                // Check if the MS released bash was used.  MS bash is installed under windows\system32, and it's possible somebody else
                // could be installing their bash into that directory, but not likely.  If the user first runs a non-MS bash,
                // and then runs MS bash, we won't detect the MS bash as running, but it's an acceptable compromise.
                if (!_fBashUsed && !_wcsnicmp(c_pwszBashExeName, pwszFileName, MAX_PATH))
                {
                    // We could have gotten the system directory once when this class starts, but we'd have to hold the memory for it
                    // plus we're not sure we'd ever need it, so just get it when we know we're running bash.exe.
                    WCHAR wszSystemDirectory[MAX_PATH] = L"";
                    if (GetSystemDirectory(wszSystemDirectory, ARRAYSIZE(wszSystemDirectory)))
                    {
                        _fBashUsed = (PathIsSameRoot(wszFilePathAndName, wszSystemDirectory) == TRUE);
                    }
                }

                // In order to send out a dynamic array of strings through telemetry, we have to pack the strings into a single WCHAR array.
                // There currently aren't any helper functions for this, and we have to pack it manually.
                // To understand the format of the single string, consult the documentation in the traceloggingprovider.h file.
                if (SUCCEEDED(StringCchCopyW(_wchProcessFileNames + _iProcessFileNamesNext, ARRAYSIZE(_wchProcessFileNames) - _iProcessFileNamesNext - 1, pwszFileName)))
                {
                    // As each FileName comes in, it's appended to the end.  However to improve searching speed, we have an array of indexes
                    // that is alphabetically sorted.  We could call qsort, but that would be a waste in performance since we're just adding one string
                    // at a time and we always keep the array sorted, so just shift everything over one.
                    for (size_t n = _uiNumberProcessFileNames; n > iFileName; n--)
                    {
                        _rgiAlphabeticalIndex[n] = _rgiAlphabeticalIndex[n - 1];
                    }

                    // Now point to the string, and set the count to 1.
                    _rgiAlphabeticalIndex[iFileName] = _uiNumberProcessFileNames;
                    _rgiProccessFileNameIndex[_uiNumberProcessFileNames] = _iProcessFileNamesNext;
                    _rguiProcessFileNamesCount[_uiNumberProcessFileNames] = 1;
                    _iProcessFileNamesNext += wcslen(pwszFileName) + 1;
                    _iProcessConnectedCurrently = _uiNumberProcessFileNames++;

                    // Packed arrays start with a UINT16 value indicating the number of elements in the array.
                    BYTE *pbFileNames = reinterpret_cast<BYTE*>(_wchProcessFileNames);
                    pbFileNames[0] = (BYTE)_uiNumberProcessFileNames;
                    pbFileNames[1] = (BYTE)(_uiNumberProcessFileNames >> 8);
                }
            }
        }
    }
}

// This Function sends final Trace log before session closes.
// We're primarily sending this telemetry once at the end, and only when the user interacted with the console
// so we don't overwhelm our servers by sending a constant stream of telemetry while the console is being used.
void Telemetry::WriteFinalTraceLog()
{
    // This is a bit of processing, so don't do it for the 95% of machines that aren't being sampled.
    if (TraceLoggingProviderEnabled(g_hConhostV2EventTraceProvider, 0, MICROSOFT_KEYWORD_MEASURES))
    {
        // Normally we would set the activity Id earlier, but since we know the parser only sends
        // one final log at the end, setting the activity this late should be fine.
        TermTelemetry::Instance().SetActivityId(_activity.Id());
        TermTelemetry::Instance().SetShouldWriteFinalLog(_fUserInteractiveForTelemetry);

        if (_fUserInteractiveForTelemetry)
        {
            TotalCodesForPreviousProcess();

            // Send this back using "measures" since we want a good sampling of our entire userbase.
            time_t tEndedAt;
            time(&tEndedAt);
#pragma prefast(suppress:__WARNING_NONCONST_LOCAL, "Activity can't be const, since it's set to a random value on startup.")
            TraceLoggingWriteTagged(_activity,
                "SessionEnding",
                TraceLoggingBool(_fBashUsed, "BashUsed"),
                TraceLoggingBool(_fCtrlPgUpPgDnUsed, "CtrlPgUpPgDnUsed"),
                TraceLoggingBool(_fKeyboardTextEditingUsed, "KeyboardTextEditingUsed"),
                TraceLoggingBool(_fKeyboardTextSelectionUsed, "KeyboardTextSelectionUsed"),
                TraceLoggingBool(g_ciConsoleInformation.LinkTitle == nullptr, "LaunchedFromShortcut"),
                // Normally we would send out a single array containing the name and count,
                // but that's difficult to do with our telemetry system, so send out two separate arrays.
                // Casting to UINT should be fine, since our array size is only 2K.
                TraceLoggingPackedField(_wchProcessFileNames, static_cast<UINT>(sizeof(WCHAR) * _iProcessFileNamesNext), TlgInUNICODESTRING | TlgInVcount, "ProcessesConnected"),
                TraceLoggingUInt32Array(_rguiProcessFileNamesCount, _uiNumberProcessFileNames, "ProcessesConnectedCount"),
                TraceLoggingUInt32Array(_rguiProcessFileNamesCodesCount, _uiNumberProcessFileNames, "ProcessesConnectedCodesCount"),
                TraceLoggingUInt32Array(_rguiProcessFileNamesFailedCodesCount, _uiNumberProcessFileNames, "ProcessesConnectedFailedCodesCount"),
                TraceLoggingUInt32Array(_rguiProcessFileNamesFailedOutsideCodesCount, _uiNumberProcessFileNames, "ProcessesConnectedFailedOutsideCount"),
                // Send back both starting and ending times separately instead just usage time (ending - starting).
                // This can help us determine if they were using multiple consoles at the same time.
                TraceLoggingInt32(static_cast<int>(_tStartedAt), "StartedUsingAtSeconds"),
                TraceLoggingInt32(static_cast<int>(tEndedAt), "EndedUsingAtSeconds"),
                TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES));

            // Always send this back.  We could only send this back when they click "OK" in the settings dialog, but sending it
            // back every time should give us a good idea of their current, final settings, and not just only when they change a setting.
#pragma prefast(suppress:__WARNING_NONCONST_LOCAL, "Activity can't be const, since it's set to a random value on startup.")
            TraceLoggingWriteTagged(_activity,
                "Settings",
                TraceLoggingBool(g_ciConsoleInformation.GetAutoPosition(), "AutoPosition"),
                TraceLoggingBool(g_ciConsoleInformation.GetHistoryNoDup(), "HistoryNoDuplicates"),
                TraceLoggingBool(g_ciConsoleInformation.GetInsertMode(), "InsertMode"),
                TraceLoggingBool(g_ciConsoleInformation.GetLineSelection(), "LineSelection"),
                TraceLoggingBool(g_ciConsoleInformation.GetQuickEdit(), "QuickEdit"),
                TraceLoggingValue(g_ciConsoleInformation.GetWindowAlpha(), "WindowAlpha"),
                TraceLoggingBool(g_ciConsoleInformation.GetWrapText(), "WrapText"),
                TraceLoggingUInt32Array(g_ciConsoleInformation.GetColorTable(), (UINT16)g_ciConsoleInformation.GetColorTableSize(), "ColorTable"),
                TraceLoggingValue(g_ciConsoleInformation.CP, "CodePageInput"),
                TraceLoggingValue(g_ciConsoleInformation.OutputCP, "CodePageOutput"),
                TraceLoggingValue(g_ciConsoleInformation.GetFontSize().X, "FontSizeX"),
                TraceLoggingValue(g_ciConsoleInformation.GetFontSize().Y, "FontSizeY"),
                TraceLoggingValue(g_ciConsoleInformation.GetHotKey(), "HotKey"),
                TraceLoggingValue(g_ciConsoleInformation.GetScreenBufferSize().X, "ScreenBufferSizeX"),
                TraceLoggingValue(g_ciConsoleInformation.GetScreenBufferSize().Y, "ScreenBufferSizeY"),
                TraceLoggingValue(g_ciConsoleInformation.GetStartupFlags(), "StartupFlags"),
                TraceLoggingValue(g_ciConsoleInformation.GetVirtTermLevel(), "VirtualTerminalLevel"),
                TraceLoggingValue(g_ciConsoleInformation.GetWindowSize().X, "WindowSizeX"),
                TraceLoggingValue(g_ciConsoleInformation.GetWindowSize().Y, "WindowSizeY"),
                TraceLoggingValue(g_ciConsoleInformation.GetWindowOrigin().X, "WindowOriginX"),
                TraceLoggingValue(g_ciConsoleInformation.GetWindowOrigin().Y, "WindowOriginY"),
                TraceLoggingValue(g_ciConsoleInformation.GetFaceName(), "FontName"),
                TraceLoggingBool(g_ciConsoleInformation.IsAltF4CloseAllowed(), "AllowAltF4Close"),
                TraceLoggingBool(g_ciConsoleInformation.GetCtrlKeyShortcutsDisabled(), "ControlKeyShortcutsDisabled"),
                TraceLoggingBool(g_ciConsoleInformation.GetEnableColorSelection(), "EnabledColorSelection"),
                TraceLoggingBool(g_ciConsoleInformation.GetExtendedEditKey(), "ExtendedEditKey"),
                TraceLoggingBool(g_ciConsoleInformation.GetFilterOnPaste(), "FilterOnPaste"),
                TraceLoggingBool(g_ciConsoleInformation.GetTrimLeadingZeros(), "TrimLeadingZeros"),
                TraceLoggingValue(g_ciConsoleInformation.GetLaunchFaceName(), "LaunchFontName"),
                TraceLoggingValue(g_ciConsoleInformation.NumCommandHistories, "CommandHistoriesNumber"),
                TraceLoggingValue(g_ciConsoleInformation.GetCodePage(), "CodePage"),
                TraceLoggingValue(g_ciConsoleInformation.GetCursorSize(), "CursorSize"),
                TraceLoggingValue(g_ciConsoleInformation.GetFontFamily(), "FontFamily"),
                TraceLoggingValue(g_ciConsoleInformation.GetFontWeight(), "FontWeight"),
                TraceLoggingValue(g_ciConsoleInformation.GetHistoryBufferSize(), "HistoryBufferSize"),
                TraceLoggingValue(g_ciConsoleInformation.GetNumberOfHistoryBuffers(), "HistoryBuffersNumber"),
                TraceLoggingValue(g_ciConsoleInformation.GetScrollScale(), "ScrollScale"),
                TraceLoggingValue(g_ciConsoleInformation.GetFillAttribute(), "FillAttribute"),
                TraceLoggingValue(g_ciConsoleInformation.GetPopupFillAttribute(), "PopupFillAttribute"),
                TraceLoggingValue(g_ciConsoleInformation.GetShowWindow(), "ShowWindow"),
                TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES));

            // I could use the TraceLoggingUIntArray, but then we would have to know the order of the enums on the backend.
            // So just log each enum count separately with its string representation which makes it more human readable.
#pragma prefast(suppress:__WARNING_NONCONST_LOCAL, "Activity can't be const, since it's set to a random value on startup.")
            TraceLoggingWriteTagged(_activity,
                "ApiUsed",
                TraceLoggingUInt32(_rguiTimesApiUsed[AddConsoleAlias], "AddConsoleAlias"),
                TraceLoggingUInt32(_rguiTimesApiUsed[AllocConsole], "AllocConsole"),
                TraceLoggingUInt32(_rguiTimesApiUsed[AttachConsole], "AttachConsole"),
                TraceLoggingUInt32(_rguiTimesApiUsed[CreateConsoleScreenBuffer], "CreateConsoleScreenBuffer"),
                TraceLoggingUInt32(_rguiTimesApiUsed[GenerateConsoleCtrlEvent], "GenerateConsoleCtrlEvent"),
                TraceLoggingUInt32(_rguiTimesApiUsed[FillConsoleOutputAttribute], "FillConsoleOutputAttribute"),
                TraceLoggingUInt32(_rguiTimesApiUsed[FillConsoleOutputCharacter], "FillConsoleOutputCharacter"),
                TraceLoggingUInt32(_rguiTimesApiUsed[FlushConsoleInputBuffer], "FlushConsoleInputBuffer"),
                TraceLoggingUInt32(_rguiTimesApiUsed[FreeConsole], "FreeConsole"),
                TraceLoggingUInt32(_rguiTimesApiUsed[GetConsoleAlias], "GetConsoleAlias"),
                TraceLoggingUInt32(_rguiTimesApiUsed[GetConsoleAliases], "GetConsoleAliases"),
                TraceLoggingUInt32(_rguiTimesApiUsed[GetConsoleAliasExesLength], "GetConsoleAliasExesLength"),
                TraceLoggingUInt32(_rguiTimesApiUsed[GetConsoleAliasesLength], "GetConsoleAliasesLength"),
                TraceLoggingUInt32(_rguiTimesApiUsed[GetConsoleAliasExes], "GetConsoleAliasExes"),
                TraceLoggingUInt32(_rguiTimesApiUsed[GetConsoleCP], "GetConsoleCP"),
                TraceLoggingUInt32(_rguiTimesApiUsed[GetConsoleCursorInfo], "GetConsoleCursorInfo"),
                TraceLoggingUInt32(_rguiTimesApiUsed[GetConsoleDisplayMode], "GetConsoleDisplayMode"),
                TraceLoggingUInt32(_rguiTimesApiUsed[GetConsoleFontSize], "GetConsoleFontSize"),
                TraceLoggingUInt32(_rguiTimesApiUsed[GetConsoleHistoryInfo], "GetConsoleHistoryInfo"),
                TraceLoggingUInt32(_rguiTimesApiUsed[GetConsoleLangId], "GetConsoleLangId"),
                TraceLoggingUInt32(_rguiTimesApiUsed[GetConsoleMode], "GetConsoleMode"),
                TraceLoggingUInt32(_rguiTimesApiUsed[GetConsoleOriginalTitle], "GetConsoleOriginalTitle"),
                TraceLoggingUInt32(_rguiTimesApiUsed[GetConsoleOutputCP], "GetConsoleOutputCP"),
                TraceLoggingUInt32(_rguiTimesApiUsed[GetConsoleProcessList], "GetConsoleProcessList"),
                TraceLoggingUInt32(_rguiTimesApiUsed[GetConsoleScreenBufferInfoEx], "GetConsoleScreenBufferInfoEx"),
                TraceLoggingUInt32(_rguiTimesApiUsed[GetConsoleSelectionInfo], "GetConsoleSelectionInfo"),
                TraceLoggingUInt32(_rguiTimesApiUsed[GetConsoleTitle], "GetConsoleTitle"),
                TraceLoggingUInt32(_rguiTimesApiUsed[GetConsoleWindow], "GetConsoleWindow"),
                TraceLoggingUInt32(_rguiTimesApiUsed[GetCurrentConsoleFontEx], "GetCurrentConsoleFontEx"),
                TraceLoggingUInt32(_rguiTimesApiUsed[GetLargestConsoleWindowSize], "GetLargestConsoleWindowSize"),
                TraceLoggingUInt32(_rguiTimesApiUsed[GetNumberOfConsoleInputEvents], "GetNumberOfConsoleInputEvents"),
                TraceLoggingUInt32(_rguiTimesApiUsed[GetNumberOfConsoleMouseButtons], "GetNumberOfConsoleMouseButtons"),
                TraceLoggingUInt32(_rguiTimesApiUsed[PeekConsoleInput], "PeekConsoleInput"),
                TraceLoggingUInt32(_rguiTimesApiUsed[ReadConsole], "ReadConsole"),
                TraceLoggingUInt32(_rguiTimesApiUsed[ReadConsoleInput], "ReadConsoleInput"),
                TraceLoggingUInt32(_rguiTimesApiUsed[ReadConsoleOutput], "ReadConsoleOutput"),
                TraceLoggingUInt32(_rguiTimesApiUsed[ReadConsoleOutputAttribute], "ReadConsoleOutputAttribute"),
                TraceLoggingUInt32(_rguiTimesApiUsed[ReadConsoleOutputCharacter], "ReadConsoleOutputCharacter"),
                TraceLoggingUInt32(_rguiTimesApiUsed[ScrollConsoleScreenBuffer], "ScrollConsoleScreenBuffer"),
                TraceLoggingUInt32(_rguiTimesApiUsed[SetConsoleActiveScreenBuffer], "SetConsoleActiveScreenBuffer"),
                TraceLoggingUInt32(_rguiTimesApiUsed[SetConsoleCP], "SetConsoleCP"),
                TraceLoggingUInt32(_rguiTimesApiUsed[SetConsoleCursorInfo], "SetConsoleCursorInfo"),
                TraceLoggingUInt32(_rguiTimesApiUsed[SetConsoleCursorPosition], "SetConsoleCursorPosition"),
                TraceLoggingUInt32(_rguiTimesApiUsed[SetConsoleDisplayMode], "SetConsoleDisplayMode"),
                TraceLoggingUInt32(_rguiTimesApiUsed[SetConsoleHistoryInfo], "SetConsoleHistoryInfo"),
                TraceLoggingUInt32(_rguiTimesApiUsed[SetConsoleMode], "SetConsoleMode"),
                TraceLoggingUInt32(_rguiTimesApiUsed[SetConsoleOutputCP], "SetConsoleOutputCP"),
                TraceLoggingUInt32(_rguiTimesApiUsed[SetConsoleScreenBufferInfoEx], "SetConsoleScreenBufferInfoEx"),
                TraceLoggingUInt32(_rguiTimesApiUsed[SetConsoleScreenBufferSize], "SetConsoleScreenBufferSize"),
                TraceLoggingUInt32(_rguiTimesApiUsed[SetConsoleTextAttribute], "SetConsoleTextAttribute"),
                TraceLoggingUInt32(_rguiTimesApiUsed[SetConsoleTitle], "SetConsoleTitle"),
                TraceLoggingUInt32(_rguiTimesApiUsed[SetConsoleWindowInfo], "SetConsoleWindowInfo"),
                TraceLoggingUInt32(_rguiTimesApiUsed[SetCurrentConsoleFontEx], "SetCurrentConsoleFontEx"),
                TraceLoggingUInt32(_rguiTimesApiUsed[WriteConsole], "WriteConsole"),
                TraceLoggingUInt32(_rguiTimesApiUsed[WriteConsoleInput], "WriteConsoleInput"),
                TraceLoggingUInt32(_rguiTimesApiUsed[WriteConsoleOutput], "WriteConsoleOutput"),
                TraceLoggingUInt32(_rguiTimesApiUsed[WriteConsoleOutputAttribute], "WriteConsoleOutputAttribute"),
                TraceLoggingUInt32(_rguiTimesApiUsed[WriteConsoleOutputCharacter], "WriteConsoleOutputCharacter"),
                TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES));

            for (int n = 0; n < ARRAYSIZE(_rguiTimesApiUsedAnsi); n++)
            {
                if (_rguiTimesApiUsedAnsi[n])
                {
                    // Ansi specific API's are used less, so check if we have anything to send back.
                    // Also breaking it up into a separate TraceLoggingWriteTagged fixes a compilation warning that
                    // the heap is too small.
#pragma prefast(suppress:__WARNING_NONCONST_LOCAL, "Activity can't be const, since it's set to a random value on startup.")
                    TraceLoggingWriteTagged(_activity,
                        "ApiAnsiUsed",
                        TraceLoggingUInt32(_rguiTimesApiUsedAnsi[AddConsoleAlias], "AddConsoleAlias"),
                        TraceLoggingUInt32(_rguiTimesApiUsedAnsi[FillConsoleOutputCharacter], "FillConsoleOutputCharacter"),
                        TraceLoggingUInt32(_rguiTimesApiUsedAnsi[GetConsoleAlias], "GetConsoleAlias"),
                        TraceLoggingUInt32(_rguiTimesApiUsedAnsi[GetConsoleAliases], "GetConsoleAliases"),
                        TraceLoggingUInt32(_rguiTimesApiUsedAnsi[GetConsoleAliasesLength], "GetConsoleAliasesLength"),
                        TraceLoggingUInt32(_rguiTimesApiUsedAnsi[GetConsoleAliasExes], "GetConsoleAliasExes"),
                        TraceLoggingUInt32(_rguiTimesApiUsedAnsi[GetConsoleAliasExesLength], "GetConsoleAliasExesLength"),
                        TraceLoggingUInt32(_rguiTimesApiUsedAnsi[GetConsoleOriginalTitle], "GetConsoleOriginalTitle"),
                        TraceLoggingUInt32(_rguiTimesApiUsedAnsi[GetConsoleTitle], "GetConsoleTitle"),
                        TraceLoggingUInt32(_rguiTimesApiUsedAnsi[PeekConsoleInput], "PeekConsoleInput"),
                        TraceLoggingUInt32(_rguiTimesApiUsedAnsi[ReadConsole], "ReadConsole"),
                        TraceLoggingUInt32(_rguiTimesApiUsedAnsi[ReadConsoleInput], "ReadConsoleInput"),
                        TraceLoggingUInt32(_rguiTimesApiUsedAnsi[ReadConsoleOutput], "ReadConsoleOutput"),
                        TraceLoggingUInt32(_rguiTimesApiUsedAnsi[ReadConsoleOutputCharacter], "ReadConsoleOutputCharacter"),
                        TraceLoggingUInt32(_rguiTimesApiUsedAnsi[SetConsoleTitle], "SetConsoleTitle"),
                        TraceLoggingUInt32(_rguiTimesApiUsedAnsi[WriteConsole], "WriteConsole"),
                        TraceLoggingUInt32(_rguiTimesApiUsedAnsi[WriteConsoleInput], "WriteConsoleInput"),
                        TraceLoggingUInt32(_rguiTimesApiUsedAnsi[WriteConsoleOutput], "WriteConsoleOutput"),
                        TraceLoggingUInt32(_rguiTimesApiUsedAnsi[WriteConsoleOutputCharacter], "WriteConsoleOutputCharacter"),
                        TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES));
                    break;
                }
            }
        }
    }
}

// Sends assert information through telemetry.
void Telemetry::LogAssert(_In_z_ const char* pszSourceText, _In_z_ const char* pszFileName, _In_ const int iLineNumber) const
{
#pragma prefast(suppress:__WARNING_NONCONST_LOCAL, "Activity can't be const, since it's set to a random value on startup.")
    TraceLoggingWriteTagged(_activity,
        "Assert",
        TraceLoggingString(pszSourceText, "SourceText"),
        TraceLoggingString(pszFileName, "FileName"),
        TraceLoggingInt32(iLineNumber, "LineNumber"),
        TraceLoggingKeyword(MICROSOFT_KEYWORD_TELEMETRY));
}

// These are legacy error messages with limited value, so don't send them back as telemetry.
void Telemetry::LogRipMessage(_In_z_ const char* pszMessage, ...) const
{
    // Code needed for passing variable parameters to the vsprintf function.
    va_list args;
    va_start(args, pszMessage);
    char szMessageEvaluated[200] = "";
    int cCharsWritten = vsprintf_s(szMessageEvaluated, ARRAYSIZE(szMessageEvaluated), pszMessage, args);
    va_end(args);

    OutputDebugStringA(szMessageEvaluated);

    if (cCharsWritten > 0)
    {
#pragma prefast(suppress:__WARNING_NONCONST_LOCAL, "Activity can't be const, since it's set to a random value on startup.")
        TraceLoggingWriteTagged(_activity,
            "RipMessage",
            TraceLoggingString(szMessageEvaluated, "Message"));
    }
}
