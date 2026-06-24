/*
    Milestone 2: a real distraction-free Markdown editor.
    Full-screen window, wide margins, 14pt Times, File menu with
    Save/Open backed by the classic File Manager. Saving straight to
    the BlueSCSI SD card (bypassing this disk's HFS volume) is a
    later milestone -- this still saves onto the boot disk itself.
*/

#include <Quickdraw.h>
#include <Windows.h>
#include <Fonts.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Events.h>
#include <OSUtils.h>
#include <ToolUtils.h>
#include <Memory.h>
#include <Files.h>
#include <StandardFile.h>
#include <SegLoad.h>
#include <Multiverse.h>
#include <string.h>

#define MARGIN_H     64
#define MARGIN_TOP   32
#define MARGIN_BOTTOM 24
#define MENU_BAR_HEIGHT 20
#define FONT_SIZE 18
#define SCROLLBAR_WIDTH 16

#define mFile    128
#define iNew     1
#define iOpen    2
#define iSave    3
#define iSaveAs  4
#define iQuit    6

#define mStyle   129
#define iBold    1
#define iItalic  2
#define iCode    3
#define iStrike  4
#define iH1      6
#define iH2      7
#define iH3      8
#define iLink    10
#define iNone    12

#define kSaveChangesAlert 130
#define kSaveBtn          1
#define kCancelBtn        2
#define kDontSaveBtn      3

#define kSplashDialog 131
#define iSplashNew    1
#define iSplashOpen   2
#define iSplashTitle  3

#define mView        130
#define iMarkdownView 1
#define iWriterView  2
#define iZoomIn      4
#define iZoomOut     5
#define iZoomDefault 6

#define MAX_STYLE_OPS 512

/* Zoom levels (point deltas from FONT_SIZE) restricted to sizes that have
   a real Times bitmap (12/14/18/24pt) -- confirmed by reading the FOND
   resource directly rather than assuming. 24pt is the largest bitmap this
   font has, so there's no graceful way to offer anything bigger. */
static short kZoomLevels[] = { -6, -4, 0, 6 };
#define kNumZoomLevels 4
#define kZoomBaselineIndex 2

#define kZoomPrefType 'ZLvl'
#define kZoomPrefID   128

static WindowPtr gWindow;
static TEHandle gTE;
static TEHandle gHiddenTE;
static TEHandle gActiveTE;
static ControlHandle gScrollBar;
static Boolean gScrollBarVisible = false;
static Boolean gDone = false;
static Boolean gHaveFile = false;
static Boolean gDirty = false;
static Str255 gFileName;
static short gVRefNum;
static MenuHandle gViewMenu;
static Boolean gHideMarkdown = true;
static short gZoomIndex = kZoomBaselineIndex;

static short CurrentFontSize(void)
{
    return FONT_SIZE + kZoomLevels[gZoomIndex];
}

static void LoadZoomPref(void)
{
    Handle prefH = GetResource(kZoomPrefType, kZoomPrefID);

    if (prefH != NULL) {
        HLock(prefH);
        gZoomIndex = *(short *) *prefH;
        HUnlock(prefH);
        ReleaseResource(prefH);
        if (gZoomIndex < 0 || gZoomIndex >= kNumZoomLevels)
            gZoomIndex = kZoomBaselineIndex;
    }
}

static void SaveZoomPref(void)
{
    Handle prefH = GetResource(kZoomPrefType, kZoomPrefID);

    if (prefH != NULL) {
        HLock(prefH);
        *(short *) *prefH = gZoomIndex;
        HUnlock(prefH);
        ChangedResource(prefH);
        WriteResource(prefH);
        ReleaseResource(prefH);
    }
}

static void Init(void)
{
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
}

/*
    Writer mode gets a black menu bar with white text; Markdown mode gets
    the standard look. There's no Menu Manager API for this on classic Mac
    OS (that's a much later Appearance Manager concept) -- on a 1-bit
    display, drawing the normal bar and then XOR-inverting that strip
    achieves the same thing trivially. Must target the Window Manager
    port (global screen coordinates), not whatever window's port happens
    to be current, since the menu bar isn't part of any window.
*/
static void UpdateMenuBarLook(void)
{
    GrafPtr savePort;
    GrafPtr wMgrPort;
    Rect bar;

    DrawMenuBar();

    if (gHideMarkdown) {
        GetPort(&savePort);
        GetWMgrPort(&wMgrPort);
        SetPort(wMgrPort);

        SetRect(&bar, 0, 0, qd.screenBits.bounds.right, MENU_BAR_HEIGHT);
        InvertRect(&bar);

        SetPort(savePort);
    }
}

static void MakeMenu(void)
{
    MenuHandle fileMenu;
    MenuHandle styleMenu;

    fileMenu = NewMenu(mFile, "\pFile");
    AppendMenu(fileMenu, "\pNew/N;Open.../O;Save/S;Save As...;(-;Quit/Q");
    InsertMenu(fileMenu, 0);

    styleMenu = NewMenu(mStyle, "\pStyle");
    AppendMenu(styleMenu, "\pBold/B;Italic/I;Code/K;Strikethrough;(-;Heading 1/1;Heading 2/2;Heading 3/3;(-;Link/L;(-;None");
    InsertMenu(styleMenu, 0);

    gViewMenu = NewMenu(mView, "\pView");
    AppendMenu(gViewMenu, "\pMarkdown;Writer;(-;Zoom In/=;Zoom Out/-;Default Size/0");
    InsertMenu(gViewMenu, 0);
    CheckItem(gViewMenu, iWriterView, true);

    UpdateMenuBarLook();
}

static void MakeWindow(void)
{
    Rect bounds;
    Rect viewRect;
    Rect sbRect;
    short fontNum;

    bounds = qd.screenBits.bounds;
    bounds.top += MENU_BAR_HEIGHT;

    gWindow = NewWindow(NULL, &bounds, "\p", true, plainDBox,
                         (WindowPtr) -1L, false, 0);
    SetPort(gWindow);

    GetFNum("\pTimes", &fontNum);
    TextFont(fontNum);
    TextSize(CurrentFontSize());

    viewRect = gWindow->portRect;
    viewRect.left += MARGIN_H;
    viewRect.right -= MARGIN_H;
    viewRect.top += MARGIN_TOP;
    viewRect.bottom -= MARGIN_BOTTOM;

    gTE = TEStyleNew(&viewRect, &viewRect);
    gHiddenTE = TEStyleNew(&viewRect, &viewRect);
    gActiveTE = gHideMarkdown ? gHiddenTE : gTE;
    TEActivate(gActiveTE);

    sbRect = viewRect;
    sbRect.left = viewRect.right + (MARGIN_H - SCROLLBAR_WIDTH) / 2;
    sbRect.right = sbRect.left + SCROLLBAR_WIDTH;
    sbRect.top -= 1;
    sbRect.bottom += 1;
    gScrollBar = NewControl(gWindow, &sbRect, "\p", false, 0, 0, 0, scrollBarProc, 0);
}

/*
    The scrollbar's value is never tracked as an independent counter --
    it's always derived fresh from TextEdit's own destRect vs. viewRect,
    so it can't drift out of sync with where the text actually is. Every
    scroll operation below scrolls by (current real offset - desired
    offset) and then re-reads the real offset afterward, rather than
    trusting an incrementally-adjusted running total.
*/
static short CurrentScrollOffset(TEHandle te)
{
    return (**te).viewRect.top - (**te).destRect.top;
}

static void SyncScrollbarToOffset(void)
{
    short newValue = CurrentScrollOffset(gActiveTE);

    /* SetControlValue always redraws the control, even when the value is
       unchanged -- called every tick, an unguarded call here would redraw
       the scrollbar (and the flicker that comes with it) on every single
       keystroke for no reason. */
    if (newValue != GetControlValue(gScrollBar))
        SetControlValue(gScrollBar, newValue);
}

/*
    Updates the scrollbar's range/visibility only -- no clamping of the
    current position. Used on the typing path, where ScrollCaretIntoView
    already owns getting the position right; re-deriving maxVal from
    TEGetHeight for a line that's actively growing as you type is exactly
    the kind of thing that could disagree with ScrollCaretIntoView's own
    (separately computed) target by a pixel or two, and clamping on that
    discrepancy every keystroke is what was causing a brief upward jump.
*/
static void UpdateScrollbarRange(void)
{
    long textHeight;
    short viewHeight;
    short maxVal;
    Boolean shouldShow;

    textHeight = TEGetHeight((**gActiveTE).nLines, 0, gActiveTE);
    viewHeight = (**gActiveTE).viewRect.bottom - (**gActiveTE).viewRect.top;

    maxVal = (textHeight > viewHeight) ? (short) (textHeight - viewHeight) : 0;

    if (maxVal != GetControlMaximum(gScrollBar))
        SetControlMaximum(gScrollBar, maxVal);

    shouldShow = (maxVal > 0);
    if (shouldShow != gScrollBarVisible) {
        if (shouldShow)
            ShowControl(gScrollBar);
        else
            HideControl(gScrollBar);
        gScrollBarVisible = shouldShow;
    }
}

/*
    Full version: also clamps the current scroll position if it now
    exceeds the (possibly shrunk) range. Needed after anything that can
    reduce content height -- Style commands, zoom, load/new, mode switch
    -- but not after plain typing, which only ever grows it.
*/
static void AdjustScrollbar(void)
{
    short maxVal;
    short curOffset;

    UpdateScrollbarRange();

    maxVal = GetControlMaximum(gScrollBar);
    curOffset = CurrentScrollOffset(gActiveTE);
    if (curOffset > maxVal)
        TEScroll(0, curOffset - maxVal, gActiveTE);
    else if (curOffset < 0)
        TEScroll(0, curOffset, gActiveTE);

    SyncScrollbarToOffset();
}

static short LineContaining(TEHandle te, short pos)
{
    short line = 0;

    while (line < (**te).nLines - 1 && (**te).lineStarts[line + 1] <= pos)
        line++;
    return line;
}

static void ScrollCaretIntoView(void)
{
    short caretLine;
    short lineTop, lineBottom;
    short viewTop, viewBottom;

    caretLine = LineContaining(gActiveTE, (**gActiveTE).selEnd);

    lineTop = (**gActiveTE).destRect.top + TEGetHeight(caretLine, 0, gActiveTE);
    lineBottom = lineTop + TEGetHeight(caretLine + 1, caretLine, gActiveTE);

    viewTop = (**gActiveTE).viewRect.top;
    viewBottom = (**gActiveTE).viewRect.bottom;

    if (lineBottom > viewBottom)
        TEScroll(0, viewBottom - lineBottom, gActiveTE);
    else if (lineTop < viewTop)
        TEScroll(0, viewTop - lineTop, gActiveTE);

    SyncScrollbarToOffset();
}

static pascal void ScrollAction(ControlHandle control, short part)
{
    short max, delta, desired;
    short pageSize;

    if (part == 0)
        return;

    max = GetControlMaximum(control);
    pageSize = (**gActiveTE).viewRect.bottom - (**gActiveTE).viewRect.top;

    switch (part) {
        case inUpButton:   delta = -16; break;
        case inDownButton: delta = 16; break;
        case inPageUp:     delta = -pageSize; break;
        case inPageDown:   delta = pageSize; break;
        default:           delta = 0; break;
    }

    desired = CurrentScrollOffset(gActiveTE) + delta;
    if (desired < 0) desired = 0;
    if (desired > max) desired = max;

    TEScroll(0, CurrentScrollOffset(gActiveTE) - desired, gActiveTE);
    SetControlValue(control, CurrentScrollOffset(gActiveTE));
}

static void DoScrollClick(Point pt)
{
    ControlHandle control;
    short part;
    short desired;

    part = FindControl(pt, gWindow, &control);
    if (part == 0 || control != gScrollBar)
        return;

    if (part == inThumb) {
        TrackControl(gScrollBar, pt, NULL);
        desired = GetControlValue(gScrollBar);
        TEScroll(0, CurrentScrollOffset(gActiveTE) - desired, gActiveTE);
        SyncScrollbarToOffset();
    } else {
        TrackControl(gScrollBar, pt, NewControlActionUPP(ScrollAction));
    }
}

static void DoUpdate(WindowPtr w)
{
    BeginUpdate(w);
    EraseRect(&w->portRect);
    TEUpdate(&w->portRect, gActiveTE);
    DrawControls(w);
    EndUpdate(w);
}

/*
    Markdown mode shows raw syntax with no visual styling at all -- just
    plain uniform text at the current zoom size. Selection is preserved
    since this gets called after Style-menu edits that already placed
    the caret somewhere meaningful.
*/
static void ClearStyles(void)
{
    TextStyle ts;
    short fontNum;
    short savedStart = (**gTE).selStart;
    short savedEnd = (**gTE).selEnd;

    GetFNum("\pTimes", &fontNum);
    ts.tsFont = fontNum;
    ts.tsFace = normal;
    ts.tsSize = CurrentFontSize();
    ts.tsColor.red = ts.tsColor.green = ts.tsColor.blue = 0;

    TESetSelect(0, 32767, gTE);
    TESetStyle(doFont + doFace + doSize + doColor, &ts, true, gTE);

    TESetSelect(savedStart, savedEnd, gTE);
}

typedef struct {
    short start, end, kind, level;
} StyleOp;

/*
    Builds gHiddenTE from gTE's canonical markdown text, stripping the
    delimiter characters themselves (**, *, `, [](), leading #s) and
    recording where the surviving text landed so styling can be applied
    afterward, in the stripped buffer's own coordinates.
*/
/*
    gTE and gHiddenTE are both bound to gWindow (a TE record draws into
    whatever GrafPort was current at TEStyleNew time, for its whole
    lifetime, regardless of which one is "active" later) -- so editing
    the *inactive* record still paints onto the window. Moving its
    viewRect off-screen for the duration of a rebuild makes those calls
    draw nothing, since drawing is clipped to viewRect every time.
*/
#define OFFSCREEN_COORD (-32000)

static void SuppressDrawing(TEHandle te, Rect *saved)
{
    *saved = (**te).viewRect;
    SetRect(&(**te).viewRect, OFFSCREEN_COORD, OFFSCREEN_COORD,
            OFFSCREEN_COORD + 100, OFFSCREEN_COORD + 100);
}

static void RestoreDrawing(TEHandle te, Rect *saved)
{
    (**te).viewRect = *saved;
}

static void BuildHiddenView(void)
{
    Handle srcH;
    long len;
    Handle outH;
    long outLen;
    long i;
    static StyleOp ops[MAX_STYLE_OPS];
    short opCount;
    short fontNum;
    TextStyle ts;
    short k;
    Rect savedViewRect;

    opCount = 0;
    srcH = (**gTE).hText;
    len = (**gTE).teLength;
    outH = NewHandle(len + 1);
    outLen = 0;

    HLock(srcH);
    HLock(outH);

    i = 0;
    while (i < len) {
        if (i == 0 || (*srcH)[i - 1] == '\r') {
            short level = 0;

            while (level < 3 && i + level < len && (*srcH)[i + level] == '#')
                level++;
            if (level > 0 && i + level < len && (*srcH)[i + level] == ' ') {
                long lineStart = i + level + 1;
                long lineEnd = lineStart;
                long outStart = outLen;

                while (lineEnd < len && (*srcH)[lineEnd] != '\r') {
                    (*outH)[outLen++] = (*srcH)[lineEnd];
                    lineEnd++;
                }
                if (opCount < MAX_STYLE_OPS) {
                    ops[opCount].start = (short) outStart;
                    ops[opCount].end = (short) outLen;
                    ops[opCount].kind = 'H';
                    ops[opCount].level = level;
                    opCount++;
                }
                i = lineEnd;
                continue;
            }
        }

        if (i + 1 < len && (*srcH)[i] == '*' && (*srcH)[i + 1] == '*') {
            long j = i + 2;

            while (j + 1 < len && !((*srcH)[j] == '*' && (*srcH)[j + 1] == '*'))
                j++;
            if (j + 1 < len) {
                long outStart = outLen, m;

                for (m = i + 2; m < j; m++)
                    (*outH)[outLen++] = (*srcH)[m];
                if (opCount < MAX_STYLE_OPS) {
                    ops[opCount].start = (short) outStart;
                    ops[opCount].end = (short) outLen;
                    ops[opCount].kind = 'B';
                    opCount++;
                }
                i = j + 2;
                continue;
            }
        }
        if ((*srcH)[i] == '*') {
            long j = i + 1;

            while (j < len && (*srcH)[j] != '*')
                j++;
            if (j < len) {
                long outStart = outLen, m;

                for (m = i + 1; m < j; m++)
                    (*outH)[outLen++] = (*srcH)[m];
                if (opCount < MAX_STYLE_OPS) {
                    ops[opCount].start = (short) outStart;
                    ops[opCount].end = (short) outLen;
                    ops[opCount].kind = 'I';
                    opCount++;
                }
                i = j + 1;
                continue;
            }
        }
        if ((*srcH)[i] == '`') {
            long j = i + 1;

            while (j < len && (*srcH)[j] != '`')
                j++;
            if (j < len) {
                long outStart = outLen, m;

                for (m = i + 1; m < j; m++)
                    (*outH)[outLen++] = (*srcH)[m];
                if (opCount < MAX_STYLE_OPS) {
                    ops[opCount].start = (short) outStart;
                    ops[opCount].end = (short) outLen;
                    ops[opCount].kind = 'C';
                    opCount++;
                }
                i = j + 1;
                continue;
            }
        }
        if ((*srcH)[i] == '[') {
            long closeBracket = i + 1;

            while (closeBracket < len && (*srcH)[closeBracket] != ']')
                closeBracket++;
            if (closeBracket < len && closeBracket + 1 < len && (*srcH)[closeBracket + 1] == '(') {
                long closeParen = closeBracket + 2;

                while (closeParen < len && (*srcH)[closeParen] != ')')
                    closeParen++;
                if (closeParen < len) {
                    long outStart = outLen, m;

                    for (m = i + 1; m < closeBracket; m++)
                        (*outH)[outLen++] = (*srcH)[m];
                    if (opCount < MAX_STYLE_OPS) {
                        ops[opCount].start = (short) outStart;
                        ops[opCount].end = (short) outLen;
                        ops[opCount].kind = 'L';
                        opCount++;
                    }
                    i = closeParen + 1;
                    continue;
                }
            }
        }

        (*outH)[outLen++] = (*srcH)[i];
        i++;
    }

    HUnlock(srcH);
    HUnlock(outH);

    SuppressDrawing(gHiddenTE, &savedViewRect);

    TESetSelect(0, 32767, gHiddenTE);
    TEDelete(gHiddenTE);
    TEInsert(*outH, outLen, gHiddenTE);
    DisposeHandle(outH);

    GetFNum("\pTimes", &fontNum);
    ts.tsFont = fontNum;
    ts.tsFace = normal;
    ts.tsSize = CurrentFontSize();
    ts.tsColor.red = ts.tsColor.green = ts.tsColor.blue = 0;
    TESetSelect(0, 32767, gHiddenTE);
    TESetStyle(doFont + doFace + doSize + doColor, &ts, true, gHiddenTE);

    for (k = 0; k < opCount; k++) {
        TextStyle opStyle;

        TESetSelect(ops[k].start, ops[k].end, gHiddenTE);
        switch (ops[k].kind) {
            case 'B':
                opStyle.tsFace = bold;
                TESetStyle(doFace, &opStyle, true, gHiddenTE);
                break;
            case 'I':
                opStyle.tsFace = italic;
                TESetStyle(doFace, &opStyle, true, gHiddenTE);
                break;
            case 'C':
                GetFNum("\pMonaco", &opStyle.tsFont);
                TESetStyle(doFont, &opStyle, true, gHiddenTE);
                break;
            case 'L':
                opStyle.tsFace = underline;
                TESetStyle(doFace, &opStyle, true, gHiddenTE);
                break;
            case 'H':
                opStyle.tsFace = bold;
                opStyle.tsSize = CurrentFontSize() + (4 - ops[k].level) * 4;
                TESetStyle(doFace + doSize, &opStyle, true, gHiddenTE);
                break;
        }
    }

    TESetSelect(0, 0, gHiddenTE);

    RestoreDrawing(gHiddenTE, &savedViewRect);
}

/*
    Reverse direction: walks gHiddenTE's text + style runs and re-derives
    markdown delimiters, rebuilding gTE's canonical text from scratch.
    Headings are detected per-line (bold + a heading-sized run at the
    line's start); everything else is inline bold/italic/Monaco-as-code.
    Link underlines round-trip as "[text]()" -- the URL itself isn't
    recoverable from styling alone, so it comes back empty; fill it in
    after switching to Markdown view.
*/
static void SyncHiddenToCanonical(void)
{
    Handle srcH;
    long len;
    Handle outH;
    long outCap;
    long outLen;
    long lineStart;
    short monacoFont;
    Rect savedViewRect;

    srcH = (**gHiddenTE).hText;
    len = (**gHiddenTE).teLength;
    outCap = len * 2 + 64;
    outH = NewHandle(outCap);
    outLen = 0;

    GetFNum("\pMonaco", &monacoFont);

    HLock(srcH);
    HLock(outH);

    lineStart = 0;
    while (lineStart <= len) {
        long lineEnd = lineStart;
        short headingLevel = 0;
        Boolean isHeading = false;

        while (lineEnd < len && (*srcH)[lineEnd] != '\r')
            lineEnd++;

        if (lineEnd > lineStart) {
            TextStyle firstStyle;
            short dummyLH, dummyFA;

            TEGetStyle((short) lineStart, &firstStyle, &dummyLH, &dummyFA, gHiddenTE);
            if (firstStyle.tsFace & bold) {
                short lvl;

                for (lvl = 1; lvl <= 3; lvl++) {
                    if (firstStyle.tsSize == CurrentFontSize() + (4 - lvl) * 4) {
                        headingLevel = lvl;
                        isHeading = true;
                        break;
                    }
                }
            }
        }

        if (isHeading) {
            short k;

            for (k = 0; k < headingLevel; k++)
                (*outH)[outLen++] = '#';
            (*outH)[outLen++] = ' ';
            BlockMove(*srcH + lineStart, *outH + outLen, lineEnd - lineStart);
            outLen += (lineEnd - lineStart);
        } else {
            long i = lineStart;
            Boolean inBold = false, inItalic = false, inCode = false;

            while (i <= lineEnd) {
                Boolean wantBold = false, wantItalic = false, wantCode = false;

                if (i < lineEnd) {
                    TextStyle st;
                    short dlh, dfa;

                    TEGetStyle((short) i, &st, &dlh, &dfa, gHiddenTE);
                    wantBold = (st.tsFace & bold) != 0;
                    wantItalic = (st.tsFace & italic) != 0;
                    wantCode = (st.tsFont == monacoFont);
                }

                if (inCode && !wantCode) { (*outH)[outLen++] = '`'; inCode = false; }
                if (inItalic && !wantItalic) { (*outH)[outLen++] = '*'; inItalic = false; }
                if (inBold && !wantBold) {
                    (*outH)[outLen++] = '*';
                    (*outH)[outLen++] = '*';
                    inBold = false;
                }

                if (!inBold && wantBold) {
                    (*outH)[outLen++] = '*';
                    (*outH)[outLen++] = '*';
                    inBold = true;
                }
                if (!inItalic && wantItalic) { (*outH)[outLen++] = '*'; inItalic = true; }
                if (!inCode && wantCode) { (*outH)[outLen++] = '`'; inCode = true; }

                if (i < lineEnd)
                    (*outH)[outLen++] = (*srcH)[i];
                i++;
            }
        }

        if (lineEnd < len)
            (*outH)[outLen++] = '\r';
        lineStart = lineEnd + 1;
    }

    HUnlock(srcH);
    HUnlock(outH);

    SuppressDrawing(gTE, &savedViewRect);

    TESetSelect(0, 32767, gTE);
    TEDelete(gTE);
    TEInsert(*outH, outLen, gTE);
    DisposeHandle(outH);

    ClearStyles();

    RestoreDrawing(gTE, &savedViewRect);
}

/*
    Remaps any run whose size matches one of the OLD base/heading sizes
    to the corresponding NEW size, in place -- used for zoom, so it
    never re-parses markdown and can't clobber unsynced edits in
    whichever buffer isn't currently canonical.
*/
static void RescaleStyles(TEHandle te, short oldBase, short newBase)
{
    long len = (**te).teLength;
    long i = 0;
    short savedStart = (**te).selStart;
    short savedEnd = (**te).selEnd;

    while (i < len) {
        TextStyle st;
        short lh, fa;
        long runStart = i;
        short oldSize;
        short newSize;

        TEGetStyle((short) i, &st, &lh, &fa, te);
        oldSize = st.tsSize;

        while (i < len) {
            TextStyle st2;

            TEGetStyle((short) i, &st2, &lh, &fa, te);
            if (st2.tsSize != oldSize)
                break;
            i++;
        }

        if (oldSize == oldBase) newSize = newBase;
        else if (oldSize == oldBase + 12) newSize = newBase + 12;
        else if (oldSize == oldBase + 8) newSize = newBase + 8;
        else if (oldSize == oldBase + 4) newSize = newBase + 4;
        else newSize = oldSize + (newBase - oldBase);

        if (newSize != oldSize) {
            TextStyle ts;

            ts.tsSize = newSize;
            TESetSelect((short) runStart, (short) i, te);
            TESetStyle(doSize, &ts, true, te);
        }
    }

    TESetSelect(savedStart, savedEnd, te);
}

static void ApplyZoomIndex(short newIndex)
{
    short oldBase;
    short newBase;

    if (newIndex < 0 || newIndex >= kNumZoomLevels || newIndex == gZoomIndex)
        return;

    oldBase = CurrentFontSize();
    gZoomIndex = newIndex;
    newBase = CurrentFontSize();

    ClearStyles();
    RescaleStyles(gHiddenTE, oldBase, newBase);
    SaveZoomPref();
    AdjustScrollbar();
    InvalRect(&gWindow->portRect);
}

static void DoZoom(short direction)
{
    ApplyZoomIndex(gZoomIndex + direction);
}

static void DoZoomReset(void)
{
    ApplyZoomIndex(kZoomBaselineIndex);
}

static void RefreshActiveView(void)
{
    if (gHideMarkdown)
        BuildHiddenView();
    else
        ClearStyles();
}

static void SetViewMode(Boolean hideMarkdown)
{
    if (hideMarkdown == gHideMarkdown)
        return;

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
    RefreshActiveView();
    AdjustScrollbar();
    InvalRect(&gWindow->portRect);
}

static void DoStartupOpen(void)
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

static Boolean DoSaveAs(void)
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

static Boolean DoSave(void)
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

static Boolean ConfirmDiscardChanges(void)
{
    if (!gDirty)
        return true;

    switch (AskSaveChanges()) {
        case kSaveBtn:     return DoSave();
        case kDontSaveBtn: return true;
        default:            return false;
    }
}

static Boolean DoOpenFile(void)
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

static void DoNewFile(void)
{
    TESetSelect(0, 32767, gTE);
    TEDelete(gTE);
    gHaveFile = false;
    gDirty = false;
    RefreshActiveView();
    AdjustScrollbar();
    InvalRect(&gWindow->portRect);
}

static void WrapSelection(char *prefix, char *suffix)
{
    short selStart, selEnd;
    long selLen, totalLen, textLen;
    short prefixLen, suffixLen;
    Handle textH;
    Handle newH;
    Boolean outerWrapped, innerWrapped;

    selStart = (**gTE).selStart;
    selEnd = (**gTE).selEnd;
    selLen = selEnd - selStart;
    textH = (**gTE).hText;
    textLen = (**gTE).teLength;

    gDirty = true;

    prefixLen = strlen(prefix);
    suffixLen = strlen(suffix);

    HLock(textH);
    outerWrapped =
        (selStart >= prefixLen) &&
        (selEnd + suffixLen <= textLen) &&
        (memcmp(*textH + selStart - prefixLen, prefix, prefixLen) == 0) &&
        (memcmp(*textH + selEnd, suffix, suffixLen) == 0);
    innerWrapped = !outerWrapped &&
        (selLen >= prefixLen + suffixLen) &&
        (memcmp(*textH + selStart, prefix, prefixLen) == 0) &&
        (memcmp(*textH + selEnd - suffixLen, suffix, suffixLen) == 0);
    HUnlock(textH);

    if (outerWrapped) {
        /* markers sit just outside the selection -- strip them (toggle off) */
        newH = NewHandle(selLen);
        HLock(newH);
        HLock(textH);
        BlockMove(*textH + selStart, *newH, selLen);
        HUnlock(textH);

        TESetSelect(selStart - prefixLen, selEnd + suffixLen, gTE);
        TEDelete(gTE);
        TEInsert(*newH, selLen, gTE);
        HUnlock(newH);
        DisposeHandle(newH);

        TESetSelect(selStart - prefixLen, selStart - prefixLen + selLen, gTE);
        return;
    }

    if (innerWrapped) {
        /* markers are part of the selection itself -- strip them (toggle off) */
        long innerLen = selLen - prefixLen - suffixLen;

        newH = NewHandle(innerLen);
        HLock(newH);
        HLock(textH);
        BlockMove(*textH + selStart + prefixLen, *newH, innerLen);
        HUnlock(textH);

        TEDelete(gTE);
        TEInsert(*newH, innerLen, gTE);
        HUnlock(newH);
        DisposeHandle(newH);

        TESetSelect(selStart, selStart + innerLen, gTE);
        return;
    }

    totalLen = prefixLen + selLen + suffixLen;
    newH = NewHandle(totalLen);
    HLock(newH);
    HLock(textH);
    BlockMove(prefix, *newH, prefixLen);
    BlockMove(*textH + selStart, *newH + prefixLen, selLen);
    BlockMove(suffix, *newH + prefixLen + selLen, suffixLen);
    HUnlock(textH);

    TEDelete(gTE);
    TEInsert(*newH, totalLen, gTE);
    HUnlock(newH);
    DisposeHandle(newH);

    TESetSelect(selStart + prefixLen, selStart + prefixLen + selLen, gTE);
}

static void ApplyHeading(short level)
{
    short selStart;
    short lineStart;
    long textLen;
    Handle textH;
    char prefix[8];
    short i;
    Boolean alreadyHeading;

    gDirty = true;

    selStart = (**gTE).selStart;
    textH = (**gTE).hText;
    textLen = (**gTE).teLength;

    lineStart = selStart;
    HLock(textH);
    while (lineStart > 0 && (*textH)[lineStart - 1] != '\r')
        lineStart--;
    HUnlock(textH);

    for (i = 0; i < level; i++)
        prefix[i] = '#';
    prefix[level] = ' ';

    HLock(textH);
    alreadyHeading =
        (lineStart + level + 1 <= textLen) &&
        (memcmp(*textH + lineStart, prefix, level + 1) == 0);
    HUnlock(textH);

    if (alreadyHeading) {
        TESetSelect(lineStart, lineStart + level + 1, gTE);
        TEDelete(gTE);
        return;
    }

    TESetSelect(lineStart, lineStart, gTE);
    TEInsert(prefix, level + 1, gTE);
}

static void DoLink(void)
{
    short selStart, selEnd;
    long selLen, totalLen;
    Handle textH;
    Handle newH;
    static char mid[] = "]()";
    short midLen = 3;
    short cursorPos;

    gDirty = true;

    selStart = (**gTE).selStart;
    selEnd = (**gTE).selEnd;
    selLen = selEnd - selStart;
    textH = (**gTE).hText;

    totalLen = 1 + selLen + midLen;
    newH = NewHandle(totalLen);
    HLock(newH);
    HLock(textH);
    (*newH)[0] = '[';
    BlockMove(*textH + selStart, *newH + 1, selLen);
    BlockMove(mid, *newH + 1 + selLen, midLen);
    HUnlock(textH);

    TEDelete(gTE);
    TEInsert(*newH, totalLen, gTE);
    HUnlock(newH);
    DisposeHandle(newH);

    cursorPos = selStart + selLen + 3;
    TESetSelect(cursorPos, cursorPos, gTE);
}

/*
    Style commands while in Hide Markdown mode apply real TextStyle
    directly to gHiddenTE instead of inserting delimiter text -- there's
    no visible syntax to insert. Toggle state is read back from the
    style at the selection start.
*/
static Boolean SelectionHasFace(Style face)
{
    TextStyle ts;
    short lh, fa;

    TEGetStyle((**gHiddenTE).selStart, &ts, &lh, &fa, gHiddenTE);
    return (ts.tsFace & face) != 0;
}

static void ToggleFace(Style face)
{
    TextStyle ts;

    ts.tsFace = SelectionHasFace(face) ? normal : face;
    TESetStyle(doFace, &ts, true, gHiddenTE);
}

static void ToggleCode(void)
{
    TextStyle ts;
    short lh, fa;
    short monacoFont, timesFont;

    GetFNum("\pMonaco", &monacoFont);
    GetFNum("\pTimes", &timesFont);

    TEGetStyle((**gHiddenTE).selStart, &ts, &lh, &fa, gHiddenTE);
    ts.tsFont = (ts.tsFont == monacoFont) ? timesFont : monacoFont;
    TESetStyle(doFont, &ts, true, gHiddenTE);
}

static void ToggleHeadingHidden(short level)
{
    short selStart;
    long lineStart, lineEnd;
    Handle textH;
    long len;
    TextStyle ts;
    short lh, fa;
    Boolean isThisLevel;

    selStart = (**gHiddenTE).selStart;
    textH = (**gHiddenTE).hText;
    len = (**gHiddenTE).teLength;

    HLock(textH);
    lineStart = selStart;
    while (lineStart > 0 && (*textH)[lineStart - 1] != '\r')
        lineStart--;
    lineEnd = lineStart;
    while (lineEnd < len && (*textH)[lineEnd] != '\r')
        lineEnd++;
    HUnlock(textH);

    TEGetStyle((short) lineStart, &ts, &lh, &fa, gHiddenTE);
    isThisLevel = (ts.tsFace & bold) && (ts.tsSize == CurrentFontSize() + (4 - level) * 4);

    TESetSelect((short) lineStart, (short) lineEnd, gHiddenTE);
    if (isThisLevel) {
        ts.tsFace = normal;
        ts.tsSize = CurrentFontSize();
    } else {
        ts.tsFace = bold;
        ts.tsSize = CurrentFontSize() + (4 - level) * 4;
    }
    TESetStyle(doFace + doSize, &ts, true, gHiddenTE);
}

/*
    Sets the style at a zero-length selection (the insertion point) --
    Style TextEdit uses this as the style for whatever gets typed next,
    which is exactly what's needed after closing a live-converted span
    so typing doesn't keep inheriting bold/italic/code indefinitely.
*/
static void SetTypingStyleNormal(short pos)
{
    TextStyle ts;
    short fontNum;

    GetFNum("\pTimes", &fontNum);
    ts.tsFont = fontNum;
    ts.tsFace = normal;
    ts.tsSize = CurrentFontSize();
    TESetSelect(pos, pos, gHiddenTE);
    TESetStyle(doFont + doFace + doSize, &ts, true, gHiddenTE);
}

/*
    Live "type the markdown, get the formatting" for Writer mode: called
    after every keystroke. Looks backward from the caret for a delimiter
    pair that the just-typed character completed, and if found, strips
    both delimiters and applies the corresponding style in place. Only
    bold/italic/code/headings are detected this way -- links need a URL
    that has nowhere to live once the brackets are gone, and strikethrough
    has no native classic Mac text style, so both stay menu-only.
*/
static void DetectInlineMarkdown(char justTyped)
{
    Handle textH;
    long len;
    long caret;
    long lineStart;

    if (justTyped == '\r') {
        SetTypingStyleNormal((**gHiddenTE).selEnd);
        return;
    }

    textH = (**gHiddenTE).hText;
    len = (**gHiddenTE).teLength;
    caret = (**gHiddenTE).selEnd;

    HLock(textH);

    lineStart = caret;
    while (lineStart > 0 && (*textH)[lineStart - 1] != '\r')
        lineStart--;

    if (justTyped == ' ') {
        short level = 0;
        long p = lineStart;

        while (level < 3 && p < caret - 1 && (*textH)[p] == '#') {
            level++;
            p++;
        }
        if (level > 0 && p == caret - 1) {
            TextStyle ts;

            HUnlock(textH);
            TESetSelect((short) lineStart, (short) caret, gHiddenTE);
            TEDelete(gHiddenTE);
            TESetSelect((short) lineStart, (short) lineStart, gHiddenTE);
            ts.tsFace = bold;
            ts.tsSize = CurrentFontSize() + (4 - level) * 4;
            TESetStyle(doFace + doSize, &ts, true, gHiddenTE);
            return;
        }
    } else if (justTyped == '*') {
        if (caret >= 4 && (*textH)[caret - 2] == '*' && (*textH)[caret - 1] == '*') {
            long p = caret - 4;

            while (p >= lineStart) {
                if ((*textH)[p] == '*' && (*textH)[p + 1] == '*' && p + 2 < caret - 2) {
                    long innerStart = p + 2;
                    long innerEnd = caret - 2;
                    TextStyle ts;

                    HUnlock(textH);
                    TESetSelect((short) innerEnd, (short) caret, gHiddenTE);
                    TEDelete(gHiddenTE);
                    TESetSelect((short) p, (short) innerStart, gHiddenTE);
                    TEDelete(gHiddenTE);

                    ts.tsFace = bold;
                    TESetSelect((short) p, (short) (innerEnd - 2), gHiddenTE);
                    TESetStyle(doFace, &ts, true, gHiddenTE);
                    SetTypingStyleNormal((short) (innerEnd - 2));
                    return;
                }
                p--;
            }
        } else if (caret >= 3 && (*textH)[caret - 2] != '*') {
            long p = caret - 2;

            while (p >= lineStart) {
                if ((*textH)[p] == '*' &&
                    (p == lineStart || (*textH)[p - 1] != '*') &&
                    (*textH)[p + 1] != '*' && p + 1 < caret - 1) {
                    long innerStart = p + 1;
                    long innerEnd = caret - 1;
                    TextStyle ts;

                    HUnlock(textH);
                    TESetSelect((short) innerEnd, (short) caret, gHiddenTE);
                    TEDelete(gHiddenTE);
                    TESetSelect((short) p, (short) innerStart, gHiddenTE);
                    TEDelete(gHiddenTE);

                    ts.tsFace = italic;
                    TESetSelect((short) p, (short) (innerEnd - 1), gHiddenTE);
                    TESetStyle(doFace, &ts, true, gHiddenTE);
                    SetTypingStyleNormal((short) (innerEnd - 1));
                    return;
                }
                p--;
            }
        }
    } else if (justTyped == '`') {
        long p = caret - 2;

        while (p >= lineStart) {
            if ((*textH)[p] == '`' && p + 1 < caret - 1) {
                long innerStart = p + 1;
                long innerEnd = caret - 1;
                TextStyle ts;

                HUnlock(textH);
                TESetSelect((short) innerEnd, (short) caret, gHiddenTE);
                TEDelete(gHiddenTE);
                TESetSelect((short) p, (short) innerStart, gHiddenTE);
                TEDelete(gHiddenTE);

                GetFNum("\pMonaco", &ts.tsFont);
                TESetSelect((short) p, (short) (innerEnd - 1), gHiddenTE);
                TESetStyle(doFont, &ts, true, gHiddenTE);
                SetTypingStyleNormal((short) (innerEnd - 1));
                return;
            }
            p--;
        }
    }

    HUnlock(textH);
}

/* "None" in Writer mode: just clear the applied style on the selection. */
static void ClearSelectionStyleHidden(void)
{
    TextStyle ts;
    short fontNum;

    if ((**gHiddenTE).selStart == (**gHiddenTE).selEnd)
        return;

    GetFNum("\pTimes", &fontNum);
    ts.tsFont = fontNum;
    ts.tsFace = normal;
    ts.tsSize = CurrentFontSize();
    TESetStyle(doFont + doFace + doSize, &ts, true, gHiddenTE);
}

/*
    "None" in Markdown mode: strips any matched markdown delimiter pairs
    that fall entirely within the selection. Delimiters that extend
    outside the selection are left alone -- to clear those,
    extend the selection to include them, or toggle the specific Style
    menu item that applied them.
*/
static void ClearMarkdownInSelection(void)
{
    Handle textH;
    short selStart, selEnd;
    Handle outH;
    long outLen;
    long i;

    selStart = (**gTE).selStart;
    selEnd = (**gTE).selEnd;
    if (selStart == selEnd)
        return;

    textH = (**gTE).hText;
    outH = NewHandle(selEnd - selStart + 1);
    outLen = 0;

    HLock(textH);
    HLock(outH);

    i = selStart;
    while (i < selEnd) {
        if (i == 0 || (*textH)[i - 1] == '\r') {
            short level = 0;
            long p = i;

            while (level < 3 && p < selEnd && (*textH)[p] == '#') {
                level++;
                p++;
            }
            if (level > 0 && p < selEnd && (*textH)[p] == ' ') {
                i = p + 1;
                continue;
            }
        }

        if (i + 1 < selEnd && (*textH)[i] == '*' && (*textH)[i + 1] == '*') {
            long j = i + 2;

            while (j + 1 < selEnd && !((*textH)[j] == '*' && (*textH)[j + 1] == '*'))
                j++;
            if (j + 1 < selEnd) {
                long k;

                for (k = i + 2; k < j; k++)
                    (*outH)[outLen++] = (*textH)[k];
                i = j + 2;
                continue;
            }
        }
        if ((*textH)[i] == '*') {
            long j = i + 1;

            while (j < selEnd && (*textH)[j] != '*')
                j++;
            if (j < selEnd) {
                long k;

                for (k = i + 1; k < j; k++)
                    (*outH)[outLen++] = (*textH)[k];
                i = j + 1;
                continue;
            }
        }
        if ((*textH)[i] == '`') {
            long j = i + 1;

            while (j < selEnd && (*textH)[j] != '`')
                j++;
            if (j < selEnd) {
                long k;

                for (k = i + 1; k < j; k++)
                    (*outH)[outLen++] = (*textH)[k];
                i = j + 1;
                continue;
            }
        }
        if ((*textH)[i] == '[') {
            long closeBracket = i + 1;

            while (closeBracket < selEnd && (*textH)[closeBracket] != ']')
                closeBracket++;
            if (closeBracket < selEnd && closeBracket + 1 < selEnd && (*textH)[closeBracket + 1] == '(') {
                long closeParen = closeBracket + 2;

                while (closeParen < selEnd && (*textH)[closeParen] != ')')
                    closeParen++;
                if (closeParen < selEnd) {
                    long k;

                    for (k = i + 1; k < closeBracket; k++)
                        (*outH)[outLen++] = (*textH)[k];
                    i = closeParen + 1;
                    continue;
                }
            }
        }

        (*outH)[outLen++] = (*textH)[i];
        i++;
    }

    HUnlock(textH);
    HUnlock(outH);

    TESetSelect(selStart, selEnd, gTE);
    TEDelete(gTE);
    TEInsert(*outH, outLen, gTE);
    DisposeHandle(outH);

    TESetSelect(selStart, (short) (selStart + outLen), gTE);
}

static void DoMenuCommand(long menuResult)
{
    short menuID = HiWord(menuResult);
    short menuItem = LoWord(menuResult);

    if (menuID == mFile) {
        switch (menuItem) {
            case iNew:
                if (ConfirmDiscardChanges())
                    DoNewFile();
                break;
            case iOpen:
                if (ConfirmDiscardChanges())
                    DoOpenFile();
                break;
            case iSave:   DoSave(); break;
            case iSaveAs: DoSaveAs(); break;
            case iQuit:
                if (ConfirmDiscardChanges())
                    gDone = true;
                break;
        }
    } else if (menuID == mStyle) {
        gDirty = true;
        if (gHideMarkdown) {
            switch (menuItem) {
                case iBold:   ToggleFace(bold); break;
                case iItalic: ToggleFace(italic); break;
                case iCode:   ToggleCode(); break;
                case iStrike: break; /* no native strikethrough on classic Mac text styles */
                case iH1:     ToggleHeadingHidden(1); break;
                case iH2:     ToggleHeadingHidden(2); break;
                case iH3:     ToggleHeadingHidden(3); break;
                case iLink:   ToggleFace(underline); break;
                case iNone:   ClearSelectionStyleHidden(); break;
            }
        } else {
            switch (menuItem) {
                case iBold:   WrapSelection("**", "**"); break;
                case iItalic: WrapSelection("*", "*"); break;
                case iCode:   WrapSelection("`", "`"); break;
                case iStrike: WrapSelection("~~", "~~"); break;
                case iH1:     ApplyHeading(1); break;
                case iH2:     ApplyHeading(2); break;
                case iH3:     ApplyHeading(3); break;
                case iLink:   DoLink(); break;
                case iNone:   ClearMarkdownInSelection(); break;
            }
            ClearStyles();
        }
        AdjustScrollbar();
    } else if (menuID == mView) {
        switch (menuItem) {
            case iMarkdownView: SetViewMode(false); break;
            case iWriterView:   SetViewMode(true); break;
            case iZoomIn:       DoZoom(1); break;
            case iZoomOut:      DoZoom(-1); break;
            case iZoomDefault:  DoZoomReset(); break;
        }
    }
    HiliteMenu(0);
}

static void EventLoop(void)
{
    EventRecord event;
    WindowPtr w;
    short part;

    while (!gDone) {
        if (WaitNextEvent(everyEvent, &event, 15, NULL)) {
            /* Disposing a dialog/window doesn't restore the caller's port
               -- cheap insurance against any path (found or not) leaving
               thePort dangling at a freed window's memory. */
            SetPort(gWindow);
            switch (event.what) {
                case updateEvt:
                    DoUpdate((WindowPtr) event.message);
                    break;

                case mouseDown:
                    part = FindWindow(event.where, &w);
                    if (part == inMenuBar) {
                        DoMenuCommand(MenuSelect(event.where));
                    } else if (part == inContent) {
                        ControlHandle hitControl;

                        SetPort(w);
                        GlobalToLocal(&event.where);
                        if (FindControl(event.where, w, &hitControl) != 0 && hitControl == gScrollBar)
                            DoScrollClick(event.where);
                        else
                            TEClick(event.where, (event.modifiers & shiftKey) != 0, gActiveTE);
                    }
                    break;

                case keyDown:
                case autoKey: {
                    char key = event.message & charCodeMask;
                    if (event.modifiers & cmdKey) {
                        if (event.what == keyDown)
                            DoMenuCommand(MenuKey(key));
                    } else {
                        TEKey(key, gActiveTE);
                        if (key < 0x1C || key > 0x1F) {
                            gDirty = true;
                            if (gHideMarkdown)
                                DetectInlineMarkdown(key);
                        }
                        ScrollCaretIntoView();
                        UpdateScrollbarRange();
                    }
                    break;
                }

                case activateEvt:
                    if ((event.modifiers & activeFlag) != 0)
                        TEActivate(gActiveTE);
                    else
                        TEDeactivate(gActiveTE);
                    break;
            }
        }
        TEIdle(gActiveTE);
    }
}

static pascal void DrawSplashTitle(DialogPtr dlg, short itemNo)
{
    DialogItemType type;
    Handle itemH;
    Rect box;
    short textWidth;
    Str255 s;

    GetDialogItem(dlg, itemNo, &type, &itemH, &box);
    SetPort(dlg);

    TextFont(0);
    TextSize(0);
    TextFace(bold);
    BlockMove("\pArtfulType", s, 11);
    textWidth = StringWidth(s);
    MoveTo(box.left + (box.right - box.left - textWidth) / 2, box.top + 16);
    DrawString(s);

    TextFace(normal);
    BlockMove("\pby Action Retro", s, 16);
    textWidth = StringWidth(s);
    MoveTo(box.left + (box.right - box.left - textWidth) / 2, box.top + 36);
    DrawString(s);
}

static void ShowSplashScreen(void)
{
    DialogPtr dlg;
    short item;
    DialogItemType type;
    Handle itemH;
    Rect box;
    Boolean done = false;

    while (!done) {
        dlg = GetNewDialog(kSplashDialog, NULL, (WindowPtr) -1L);
        if (dlg == NULL)
            return;

        GetDialogItem(dlg, iSplashTitle, &type, &itemH, &box);
        SetDialogItem(dlg, iSplashTitle, type, (Handle) NewUserItemUPP(DrawSplashTitle), &box);

        do {
            ModalDialog(NULL, &item);
        } while (item != iSplashNew && item != iSplashOpen);

        DisposeDialog(dlg);
        SetPort(gWindow);
        UpdateMenuBarLook();

        /* Open Document, then Cancel in the file picker -- show the splash again */
        done = (item == iSplashNew) || DoOpenFile();
    }
}

int main(void)
{
    short message, count;

    Init();
    LoadZoomPref();
    MakeMenu();
    MakeWindow();

    /* A newly-created visible window has its whole content area marked
       invalid automatically, but the splash dialog appears before the
       event loop ever gets a chance to dequeue and process that update
       event -- force the real BeginUpdate/TEUpdate/EndUpdate cycle to
       happen now, so the window has gone through one proper paint before
       the user can type anything. Without this, the very first line typed
       (before any other update has occurred) doesn't render reliably. */
    DoUpdate(gWindow);

    CountAppFiles(&message, &count);
    if (count >= 1 && message == appOpen)
        DoStartupOpen();
    else
        ShowSplashScreen();

    EventLoop();
    return 0;
}
