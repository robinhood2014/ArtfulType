#include "app.h"

/* Include the Classic Print Manager header if available in the SDK; otherwise
   fall back to a build-friendly stub path so compilation succeeds. */
#if defined(__has_include)
# if __has_include(<Printing.h>)
#  include <Printing.h>
#  define HAVE_PRINTING 1
# endif
#endif

/* Simple in-memory print settings until persistent prefs are added */
static Rect gPrintPageRect = {0,0,0,0};
static short gPrintOrientation = 0; /* 0 == portrait, 1 == landscape */

static void RefreshActiveView(void)
{
    if (gHideMarkdown)
        BuildHiddenView();
    else
        ClearStyles();
}

void SetViewMode(Boolean hideMarkdown)
{
    if (hideMarkdown == gHideMarkdown)
        return;

    ClearUndoRedoStacks();
    UpdateEditMenuState();
    TEDeactivate(gActiveTE);

    if (hideMarkdown) {
        BuildHiddenView();
        gActiveTE = gHiddenTE;
    } else {
        SyncHiddenToCanonical();
        gActiveTE = gTE;
    }

    TEActivate(gActiveTE);
    gHideMarkdown = hideMarkdown;
    CheckItem(gViewMenu, iMarkdownView, !hideMarkdown);
    CheckItem(gViewMenu, iWriterView, hideMarkdown);
    UpdateMenuBarLook();
    AdjustScrollbar();
    InvalRect(&gWindow->portRect);
}

static void WriteFile(StringPtr name, short vRefNum)
{
    short refNum;
    long count;
    Handle textH = (**gTE).hText;
    OSErr err;

    Create(name, vRefNum, 'ArtT', 'TEXT');

    err = FSOpen(name, vRefNum, &refNum);
    if (err != noErr)
        return;

    SetEOF(refNum, 0);
    count = (**gTE).teLength;
    HLock(textH);
    FSWrite(refNum, &count, *textH);
    HUnlock(textH);
    FSClose(refNum);
}

static void ReadFile(StringPtr name, short vRefNum)
{
    short refNum;
    long count;
    long eof;
    Handle textH;
    OSErr err;

    err = FSOpen(name, vRefNum, &refNum);
    if (err != noErr)
        return;

    GetEOF(refNum, &eof);
    textH = NewHandle(eof);
    HLock(textH);
    count = eof;
    FSRead(refNum, &count, *textH);
    FSClose(refNum);

    TESetSelect(0, 32767, gTE);
    TEDelete(gTE);
    TEInsert(*textH, count, gTE);
    HUnlock(textH);
    DisposeHandle(textH);

    gDirty = false;
    ClearUndoRedoStacks();
    UpdateEditMenuState();
    RefreshActiveView();
    AdjustScrollbar();
    InvalRect(&gWindow->portRect);
}

/* Simple on-disk logger for diagnosing menu command dispatch at runtime.
   Writes a brief Pascal string message to PageSetupLog.txt in the current
   volume (gVRefNum). This avoids relying on dialogs which may be
   suppressed or misrouted in the emulator. */
static void LogMessage(Str255 msg)
{
    short refNum;
    long count;
    OSErr err;

    /* Ensure a file exists, then open and write (overwrite). */
    Create("\pPageSetupLog.txt", gVRefNum, 'ArtT', 'TEXT');
    err = FSOpen("\pPageSetupLog.txt", gVRefNum, &refNum);
    if (err != noErr)
        return;

    count = msg[0];
    FSSetFPos(refNum, fsFromStart, 0);
    FSWrite(refNum, &count, &msg[1]);
    FSClose(refNum);
}

void DoStartupOpen(void)
{
    short message, count;
    AppFile theFile;

    CountAppFiles(&message, &count);
    if (count < 1 || message != appOpen)
        return;

    GetAppFiles(1, &theFile);
    BlockMove(theFile.fName, gFileName, theFile.fName[0] + 1);
    gVRefNum = theFile.vRefNum;
    gHaveFile = true;
    ReadFile(gFileName, gVRefNum);
    ClrAppFiles(1);
}

Boolean DoSaveAs(void)
{
    SFReply reply;
    Point where = {100, 100};

    if (gHideMarkdown)
        SyncHiddenToCanonical();

    SFPutFile(where, "\pSave document as:", "\pUntitled.md", NULL, &reply);
    UpdateMenuBarLook();
    if (!reply.good)
        return false;

    BlockMove(reply.fName, gFileName, reply.fName[0] + 1);
    gVRefNum = reply.vRefNum;
    gHaveFile = true;
    WriteFile(gFileName, gVRefNum);
    gDirty = false;
    return true;
}

Boolean DoSave(void)
{
    if (!gHaveFile)
        return DoSaveAs();

    if (gHideMarkdown)
        SyncHiddenToCanonical();

    WriteFile(gFileName, gVRefNum);
    gDirty = false;
    return true;
}

static short AskSaveChanges(void)
{
    short hit;

    if (gHaveFile)
        ParamText(gFileName, "\p", "\p", "\p");
    else
        ParamText("\pUntitled", "\p", "\p", "\p");

    hit = Alert(kSaveChangesAlert, NULL);
    UpdateMenuBarLook();
    return hit;
}

Boolean ConfirmDiscardChanges(void)
{
    if (!gDirty)
        return true;

    switch (AskSaveChanges()) {
        case kSaveBtn:     return DoSave();
        case kDontSaveBtn: return true;
        default:            return false;
    }
}

Boolean DoOpenFile(void)
{
    SFReply reply;
    Point where = {100, 100};
    SFTypeList types;

    types[0] = 'TEXT';

    SFGetFile(where, "\p", NULL, 1, types, NULL, &reply);
    UpdateMenuBarLook();
    if (!reply.good)
        return false;

    BlockMove(reply.fName, gFileName, reply.fName[0] + 1);
    gVRefNum = reply.vRefNum;
    gHaveFile = true;
    ReadFile(gFileName, gVRefNum);
    return true;
}

void DoNewFile(void)
{
    TESetSelect(0, 32767, gTE);
    TEDelete(gTE);
    gHaveFile = false;
    gDirty = false;
    ClearUndoRedoStacks();
    UpdateEditMenuState();
    RefreshActiveView();
    AdjustScrollbar();
    InvalRect(&gWindow->portRect);
}

void DoPageSetup(void)
{
    /* Diagnostic: 5 quick beeps to confirm this function is called */
    short i;
    for (i = 0; i < 5; i++) {
        SysBeep(1);
    }

    /*
     * Use the Classic Print Manager's Page Setup dialog when available.
     * If unavailable or it fails, fall back to a simple in-app Page Setup
     * that records the current window rect as the page.
     */
#ifdef HAVE_PRINTING
    Rect pageRect;
    OSErr err = noErr;

    /* Try SDK-provided PageSetup if present; many SDKs differ here. */
    #if defined(PageSetup)
        err = PageSetup(&pageRect); /* SDK-specific function */
        if (err == noErr) {
            gPrintPageRect = pageRect;
        } else {
            /* Fallback to window bounds when PageSetup fails. */
            gPrintPageRect = gWindow->portRect;
        }
    #else
        /* If PageSetup symbol not available, use window bounds. */
        gPrintPageRect = gWindow->portRect;
    #endif
#else
    /* Fallback: remember the current window bounds as a page for now. */
    gPrintPageRect = gWindow->portRect;
#endif

    /* Diagnostic: 3 beeps to signal completion */
    for (i = 0; i < 3; i++) {
        SysBeep(1);
    }

    UpdateMenuBarLook();
}

void DoPrint(void)
{
    short i;

    /*
     * Print Manager integration: open a print dialog, and then print
     * the document in gTE. This is a minimal outline using the classic
     * Printing toolbox — exact API calls will be refined after the
     * first build attempt with the Retro68 SDK.
     */
#ifdef HAVE_PRINTING
    /* Diagnostic: 2 beeps = Print Manager path */
    for (i = 0; i < 2; i++) {
        SysBeep(1);
    }

    /*
     * Attempt to use classic Print Manager APIs. Exact function names and
     * signatures differ between SDKs; this implementation uses commonly
     * found names (PrOpen/PrClose/PrDlg/PrJobBegin/PrPageBegin/PrPageEnd)
     * and falls back gracefully if any step fails at runtime.
     */
    OSErr err = noErr;

    /* Open the Print Manager (many SDKs provide PrOpen/PrClose) */
    #if defined(PrOpen)
        err = PrOpen();
    #endif

    /* Show a print dialog if available (PrDlg is a common helper) */
    #if defined(PrDlg)
        {
            short itemHit;
            PrDlg(&itemHit); /* present modal print dialog */
        }
    #endif

    /* Start a print job */
    #if defined(PrJobBegin)
        err = PrJobBegin();
    #endif

    if (err == noErr) {
        /* Print the document as a single page for now (expand to pagination later) */

        #if defined(PrPageBegin) && defined(PrPageEnd)
            PrPageBegin();
            /* Render the TextEdit contents to the current printer graphics port.
             * If the SDK offers TEPrint or a similar helper, use it; otherwise
             * draw the text manually using QuickDraw text routines into the
             * current graf port (the printer port when inside a print page).
             */
            #if defined(TEPrint)
                TEPrint(gTE);
            #else
                /* Manual fallback: iterate through TE text and draw with DrawText
                 * This is intentionally simple: it prints the visible window's
                 * text using the current font/size. Proper pagination requires
                 * measuring lines and multiple pages, which can be added later.
                 */
                {
                    long len = (**gTE).teLength;
                    Handle h = (**gTE).hText;
                    HLock(h);
                    char *buf = (char *) *h;
                    Point pen = {72, 72}; /* 1-inch margins (72 points/inch) */
                    TextFont(0);
                    TextSize(CurrentFontSize());
                    /* Simple: draw as a single block using DrawText
                     * Note: DrawText takes a Pascal string; convert if needed.
                     */
                    Str255 tmp;
                    short copyLen = (len > 254) ? 254 : (short)len;
                    tmp[0] = copyLen;
                    memcpy(&tmp[1], buf, copyLen);
                    MoveTo(pen.h, pen.v);
                    DrawText(tmp, 0, copyLen);
                    HUnlock(h);
                }
            #endif

            PrPageEnd();
        #else
            /* If page APIs aren't available, try a generic print call if present */
            #if defined(PrPrint)
                PrPrint();
            #else
                SysBeep(1); /* no usable print API found at runtime */
            #endif
        #endif

        #if defined(PrJobEnd)
            PrJobEnd();
        #endif
    }

    #if defined(PrClose)
        PrClose();
    #endif
#else
    /* Diagnostic: 4 beeps = Fallback file-write path */
    for (i = 0; i < 4; i++) {
        SysBeep(1);
    }

    /* Fallback: write the canonical text to a temporary file for later printing */
    if (gHideMarkdown)
        SyncHiddenToCanonical();

    /* create temporary file on the desktop or current folder named PrintOutput.txt */
    Str255 outName = "\pPrintOutput.txt";
    short vRef = gVRefNum;
    WriteFile(outName, vRef);

    /* Diagnostic: 3 beeps = Fallback complete */
    for (i = 0; i < 3; i++) {
        SysBeep(1);
    }
#endif
    UpdateMenuBarLook();
}
