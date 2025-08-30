/* ----------------------------------------------------------------------------
 * expWinConsoleDetourer.cpp --
 *
 *	Console detourer core implimentation.
 *
 * ----------------------------------------------------------------------------
 *
 * Written by: Don Libes, libes@cme.nist.gov, NIST, 12/3/90
 * 
 * Design and implementation of this program was paid for by U.S. tax
 * dollars.  Therefore it is public domain.  However, the author and NIST
 * would appreciate credit if this program or parts of it are used.
 * 
 * Copyright (c) 2001-2002 Telindustrie, LLC
 * Copyright (c) 2003 ActiveState Corporation
 *	Work by David Gravereaux <davygrvy@pobox.com> for any Win32 OS.
 *	Based on work by Gordon Chaffee <chaffee@bmrc.berkeley.edu>
 *
 * ----------------------------------------------------------------------------
 * URLs:    http://expect.nist.gov/
 *	    http://expect.sf.net/
 *	    http://bmrc.berkeley.edu/people/chaffee/expectnt.html
 * ----------------------------------------------------------------------------
 * RCS: @(#) $Id:$
 * ----------------------------------------------------------------------------
 */

#include "expWinPort.h"
#include "expWinConsoleDetourer.hpp"
#include "expWinTrampolineIPC.hpp"
#include "Detours\detours.h"
#ifdef _WIN64
#   pragma comment (lib,"detours64.lib")
#else
#   pragma comment (lib,"detours32.lib")
#endif


/* Fix the error in winnt.h */
#if (_MSC_VER == 1200 && defined(DEFAULT_UNREACHABLE))
#   undef DEFAULT_UNREACHABLE
#   define DEFAULT_UNREACHABLE default: __assume(0)
#elif !defined(DEFAULT_UNREACHABLE)
#   define DEFAULT_UNREACHABLE default: break
#endif


//  Constructor.
ConsoleDetourer::ConsoleDetourer(
	TCHAR *_cmdline,		// commandline string (in system encoding)
	TCHAR *_env,			// environment block (in system encoding)
	TCHAR *_dir,			// startup directory (in system encoding)
					// These 3 maintain a reference until
					//  _readyUp is signaled.
	int _show,			// $exp::nt_debug, shows spawned children.
	LPCSTR _trampPath,		// location -=[ ON THE FILE SYSTEM!!! ]=-
	CMclLinkedList<Message *> &_mQ,	// parent owned linkedlist for returning data stream.
	CMclLinkedList<Message *> &_eQ,	// parent owned linkedlist for returning error stream.
	CMclEvent &_readyUp,		// set when child ready (or failed).
	ConsoleDetourerCallback &_callback
    ) :
    cmdline(_cmdline), env(_env), dir(_dir), mQ(_mQ), eQ(_eQ),
    readyUp(_readyUp), callback(_callback), show(_show),
    trampPath(_trampPath), toTrampIPC(0L), fromTrampIPC(0L),
    interacting(false), status(NO_ERROR), pid(0), 
    hMasterConsole(INVALID_HANDLE_VALUE),
    hCopyScreenBuffer(INVALID_HANDLE_VALUE)
{

    hMasterConsole = CreateFileA("CONOUT$", GENERIC_READ|GENERIC_WRITE,
	    FILE_SHARE_READ|FILE_SHARE_WRITE, 0L, OPEN_EXISTING, 0, 0L);

    hCopyScreenBuffer = CreateConsoleScreenBuffer(
	    GENERIC_READ|GENERIC_WRITE, 0, NULL,
	    CONSOLE_TEXTMODE_BUFFER, NULL);
}

ConsoleDebugger::~ConsoleDebugger()
{
    if (toTrampIPC) delete toTrampIPC;
    if (fromTrampIPC) delete fromTrampIPC;
    if (hMasterConsole != INVALID_HANDLE_VALUE) {
	CloseHandle(hMasterConsole);
    }
    if (hCopyScreenBuffer != INVALID_HANDLE_VALUE) {
	CloseHandle(hCopyScreenBuffer);
    }
}

unsigned
ConsoleDebugger::ThreadHandlerProc(void)
{
    DWORD ok, exitcode;
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    DWORD createFlags = 0;

    ZeroMemory(&pi, sizeof(PROCESS_INFORMATION));
    ZeroMemory(&si, sizeof(STARTUPINFO));
    si.cb = sizeof(STARTUPINFO);
    si.dwXCountChars = 80;
    si.dwYCountChars = 25;
    if (show) {
	si.wShowWindow = SW_SHOWNOACTIVATE;
    } else {
	si.wShowWindow = SW_HIDE;
    }
    si.dwFlags = STARTF_FORCEONFEEDBACK | STARTF_USESHOWWINDOW |
	STARTF_USECOUNTCHARS;

    createFlags = DEBUG_PROCESS |	// <- Oh, so important!
	CREATE_NEW_CONSOLE |		// Yes, please.
	CREATE_DEFAULT_ERROR_MODE |	// Is this correct so error dialogs don't pop up?
	(expWinProcs->useWide ? CREATE_UNICODE_ENVIRONMENT : 0);

#if 1
    // Starvation in multithreading occurs when a thread is unable to
    // progress or execute tasks despite being ready to run, mainly due
    // to consistent denial of access to essential resources such as the
    // CPU. This phenomenon significantly impacts the overall performance
    // and responsiveness of a multithreaded application.
    
    // if someone outside us is being a resource hog, it isn't fixed
    // by us raising our priority.  It becomes an arms race, a race to
    // the bottom.

    /* Magic env var to raise expect priority */
    if (getenv("EXPECT_SLAVE_HIGH_PRIORITY") != NULL) {
	createFlags |= ABOVE_NORMAL_PRIORITY_CLASS;
    }
#endif


    ok = expWinProcs->detourCreateProcessWithDlls(
	    0L,		// Module name (not needed).
	    cmdline,	// Command line string (must be writable!).
	    0L,		// Process handle will not be inheritable.
	    0L,		// Thread handle will not be inheritable.
	    FALSE,	// No handle inheritance.
	    createFlags,// Creation flags.
	    env,	// Use custom environment block, or parent's if NULL.
	    dir,	// Use custom starting directory, or parent's if NULL.
	    &si,	// Pointer to STARTUPINFO structure.
	    &pi,	// Pointer to PROCESS_INFORMATION structure.
	    1,
	    &trampPath,
	    NULL);


    if (!ok) {
	status = GetLastError();
	readyUp.Set();
	return 0;
    }

    pid = pi.dwProcessId;

    // The process handle is to be closed by Tcl_WaitPid.
    hRootProcess = pi.hProcess;
    CloseHandle(pi.hThread);
    
    exitcode = CommonDetourer();

#ifdef EXP_DEBUG
    expDiagLog("ConsoleDebugger::ThreadHandlerProc: %x\n",
	       exitcode);
#endif
    NotifyDone();
    return exitcode;
}

/*
 *----------------------------------------------------------------------
 *
 * ConsoleDebugger::CommonDebugger --
 *
 *	This is the function that is the debugger for all slave processes
 *
 * Results:
 *	None.  This thread exits with ExitThread() when the subprocess dies.
 *
 * Side Effects:
 *	Adds the process to the things being waited for by
 *	WaitForMultipleObjects
 *
 *----------------------------------------------------------------------
 */
DWORD
ConsoleDetourer::CommonDetourer()
{
    readyUp.Set();
    return WM_QUIT; //TODO: just a placeholder for now
}

void
ConsoleDetourer::WriteMaster(LPCSTR buf, SIZE_T len)
{
    Message *msg;

    // avoid zero byte reads!
    if (len == 0) return;

    msg = new Message;
    msg->type = Message::TYPE_NORMAL;
    msg->bytes = new BYTE [len];
    memcpy(msg->bytes, buf, len);
    msg->length = len;
#ifdef EXP_DEBUG
    {
	char b[1024];
	_snprintf(b, 1024, "WriteMaster[%d]: %s",
		len, buf);
	OutputDebugString(b);
    }
#endif
    mQ.PutOnTailOfList(msg);
    callback.AlertReadable();
}

void
ConsoleDetourer::WriteMasterWarning(LPCSTR buf, SIZE_T len)
{
    // Just report this in debugging mode
    OutputDebugString(buf);
}

void
ConsoleDetourer::WriteMasterError(LPCSTR buf, SIZE_T len, DWORD status)
{
    Message *msg;
    msg = new Message;
    msg->type = Message::TYPE_ERROR;
    msg->bytes = new BYTE [len];
    memcpy(msg->bytes, buf, len);
    msg->length = len;
    msg->status = status;
    eQ.PutOnTailOfList(msg);
}

void
ConsoleDetourer::NotifyDone()
{
    Message *msg;
    msg = new Message;
    msg->type = Message::TYPE_SLAVEDONE;
    msg->bytes = 0L;
    msg->length = 0;
    mQ.PutOnTailOfList(msg);
    callback.AlertReadable();
}

DWORD
ConsoleDetourer::Write (IPCMsg *msg)
{
    DWORD result = NO_ERROR;

    /*
     * Also check to see if the pid is already dead through other means
     * (like an outside kill). [Bug 33826]
     */
    if (injectorIPC == 0L || pidKilled) {
	result = ERROR_BROKEN_PIPE;
    } else if (! injectorIPC->Post(msg)) {
	result = injectorIPC->Status();
    }

    return result;
}

void
ConsoleDetourer::EnterInteract (HANDLE OutConsole)
{
    bpCritSec.Enter();

//    interactingConsole = OutConsole;

    // More stuff to do here... What?
    // Copy entire screen contents, how?
    // Set interactingConsole to the proper size?
    // more ???  help!

    //interacting = true;
    bpCritSec.Leave();
}

void
ConsoleDetourer::ExitInteract ()
{
//    interactingConsole = 0L;
    //interacting = false;
}

const DWORD ConsoleDebugger::KEY_CONTROL= 0;
const DWORD ConsoleDebugger::KEY_SHIFT	= 1;
const DWORD ConsoleDebugger::KEY_LSHIFT	= 1;
const DWORD ConsoleDebugger::KEY_RSHIFT	= 2;
const DWORD ConsoleDebugger::KEY_ALT	= 3;

const ConsoleDebugger::KEY_MATRIX ConsoleDebugger::ModifierKeyArray[] = {
/* Control */	{ 17,  29, 0},
/* LShift */	{ 16,  42, 0},
/* RShift */	{ 16,  54, 0},
/* Alt */	{ 18,  56, 0}
};

const ConsoleDebugger::KEY_MATRIX ConsoleDebugger::AsciiToKeyArray[] = {
/*   0 */	{ 50,   3, RIGHT_CTRL_PRESSED|SHIFT_PRESSED},
/*   1 */	{ 65,  30, RIGHT_CTRL_PRESSED},
/*   2 */	{ 66,  48, RIGHT_CTRL_PRESSED},
/*   3 */	{ 67,  46, RIGHT_CTRL_PRESSED},
/*   4 */	{ 68,  32, RIGHT_CTRL_PRESSED},
/*   5 */	{ 69,  18, RIGHT_CTRL_PRESSED},
/*   6 */	{ 70,  33, RIGHT_CTRL_PRESSED},
/*   7 */	{ 71,  34, RIGHT_CTRL_PRESSED},
/*   8 */	{ 72,  35, RIGHT_CTRL_PRESSED},
/*   9 */	{  9,  15, RIGHT_CTRL_PRESSED},
/*  10 */	{ 74,  36, RIGHT_CTRL_PRESSED},
/*  11 */	{ 75,  37, RIGHT_CTRL_PRESSED},
/*  12 */	{ 76,  38, RIGHT_CTRL_PRESSED},
/*  13 */	{ 13,  28, 0},
/*  14 */	{ 78,  49, RIGHT_CTRL_PRESSED},
/*  15 */	{ 79,  24, RIGHT_CTRL_PRESSED},
/*  16 */	{ 80,  25, RIGHT_CTRL_PRESSED},
/*  17 */	{ 81,  16, RIGHT_CTRL_PRESSED},
/*  18 */	{ 82,  19, RIGHT_CTRL_PRESSED},
/*  19 */	{ 83,  31, RIGHT_CTRL_PRESSED},
/*  20 */	{ 84,  20, RIGHT_CTRL_PRESSED},
/*  21 */	{ 85,  22, RIGHT_CTRL_PRESSED},
/*  22 */	{ 86,  47, RIGHT_CTRL_PRESSED},
/*  23 */	{ 87,  17, RIGHT_CTRL_PRESSED},
/*  24 */	{ 88,  45, RIGHT_CTRL_PRESSED},
/*  25 */	{ 89,  21, RIGHT_CTRL_PRESSED},
/*  26 */	{ 90,  44, RIGHT_CTRL_PRESSED},
/*  27 */	{219, 219, RIGHT_CTRL_PRESSED|SHIFT_PRESSED},
/*  28 */	{220, 220, RIGHT_CTRL_PRESSED|SHIFT_PRESSED},
/*  29 */	{221, 221, RIGHT_CTRL_PRESSED|SHIFT_PRESSED},
/*  30 */	{ 54,  54, RIGHT_CTRL_PRESSED|SHIFT_PRESSED},
/*  31 */	{189, 189, RIGHT_CTRL_PRESSED|SHIFT_PRESSED},
/*  32 */	{ 32,  32, 0},
/*  33 */	{ 49,  49, SHIFT_PRESSED},
/*  34 */	{222, 222, SHIFT_PRESSED},
/*  35 */	{ 51,  51, SHIFT_PRESSED},
/*  36 */	{ 52,  52, SHIFT_PRESSED},
/*  37 */	{ 53,  53, SHIFT_PRESSED},
/*  38 */	{ 55,  55, SHIFT_PRESSED},
/*  39 */	{222, 222, 0},
/*  40 */	{ 57,  57, SHIFT_PRESSED},
/*  41 */	{ 48,  48, SHIFT_PRESSED},
/*  42 */	{ 56,  56, SHIFT_PRESSED},
/*  43 */	{187, 187, SHIFT_PRESSED},
/*  44 */	{188, 188, 0},
/*  45 */	{189, 189, SHIFT_PRESSED},
/*  46 */	{190, 190, 0},
/*  47 */	{191, 191, 0},
/*  48 */	{ 48,  48, 0},
/*  49 */	{ 49,  49, 0},
/*  50 */	{ 50,   3, 0},
/*  51 */	{ 51,  51, 0},
/*  52 */	{ 52,  52, 0},
/*  53 */	{ 53,  53, 0},
/*  54 */	{ 54,  54, 0},
/*  55 */	{ 55,  55, 0},
/*  56 */	{ 56,  56, 0},
/*  57 */	{ 57,  57, 0},
/*  58 */	{186, 186, SHIFT_PRESSED},
/*  59 */	{186, 186, 0},
/*  60 */	{188, 188, SHIFT_PRESSED},
/*  61 */	{187, 187, SHIFT_PRESSED},
/*  62 */	{190, 190, SHIFT_PRESSED},
/*  63 */	{191, 191, SHIFT_PRESSED},
/*  64 */	{ 50,   3, 0},
/*  65 */	{ 65,  30, SHIFT_PRESSED},
/*  66 */	{ 66,  48, SHIFT_PRESSED},
/*  67 */	{ 67,  46, SHIFT_PRESSED},
/*  68 */	{ 68,  32, SHIFT_PRESSED},
/*  69 */	{ 69,  18, SHIFT_PRESSED},
/*  70 */	{ 70,  33, SHIFT_PRESSED},
/*  71 */	{ 71,  34, SHIFT_PRESSED},
/*  72 */	{ 72,  35, SHIFT_PRESSED},
/*  73 */	{ 73,  23, SHIFT_PRESSED},
/*  74 */	{ 74,  36, SHIFT_PRESSED},
/*  75 */	{ 75,  37, SHIFT_PRESSED},
/*  76 */	{ 76,  38, SHIFT_PRESSED},
/*  77 */	{ 77,  50, SHIFT_PRESSED},
/*  78 */	{ 78,  49, SHIFT_PRESSED},
/*  79 */	{ 79,  24, SHIFT_PRESSED},
/*  80 */	{ 80,  25, SHIFT_PRESSED},
/*  81 */	{ 81,  16, SHIFT_PRESSED},
/*  82 */	{ 82,  19, SHIFT_PRESSED},
/*  83 */	{ 83,  31, SHIFT_PRESSED},
/*  84 */	{ 84,  20, SHIFT_PRESSED},
/*  85 */	{ 85,  22, SHIFT_PRESSED},
/*  86 */	{ 86,  47, SHIFT_PRESSED},
/*  87 */	{ 87,  17, SHIFT_PRESSED},
/*  88 */	{ 88,  45, SHIFT_PRESSED},
/*  89 */	{ 89,  21, SHIFT_PRESSED},
/*  90 */	{ 90,  44, SHIFT_PRESSED},
/*  91 */	{219, 219, 0},
/*  92 */	{220, 220, 0},
/*  93 */	{221, 221, 0},
/*  94 */	{ 54,  54, SHIFT_PRESSED},
/*  95 */	{189, 189, SHIFT_PRESSED},
/*  96 */	{192, 192, 0},
/*  97 */	{ 65,  30, 0},
/*  98 */	{ 66,  48, 0},
/*  99 */	{ 67,  46, 0},
/* 100 */	{ 68,  32, 0},
/* 101 */	{ 69,  18, 0},
/* 102 */	{ 70,  33, 0},
/* 103 */	{ 71,  34, 0},
/* 104 */	{ 72,  35, 0},
/* 105 */	{ 73,  23, 0},
/* 106 */	{ 74,  36, 0},
/* 107 */	{ 75,  37, 0},
/* 108 */	{ 76,  38, 0},
/* 109 */	{ 77,  50, 0},
/* 110 */	{ 78,  49, 0},
/* 111 */	{ 79,  24, 0},
/* 112 */	{ 80,  25, 0},
/* 113 */	{ 81,  16, 0},
/* 114 */	{ 82,  19, 0},
/* 115 */	{ 83,  31, 0},
/* 116 */	{ 84,  20, 0},
/* 117 */	{ 85,  22, 0},
/* 118 */	{ 86,  47, 0},
/* 119 */	{ 87,  17, 0},
/* 120 */	{ 88,  45, 0},
/* 121 */	{ 89,  21, 0},
/* 122 */	{ 90,  44, 0},
/* 123 */	{219, 219, SHIFT_PRESSED},
/* 124 */	{220, 220, SHIFT_PRESSED},
/* 125 */	{221, 221, SHIFT_PRESSED},
/* 126 */	{192, 192, SHIFT_PRESSED},
#if 0
/* 127 */	{  8,  14, RIGHT_CTRL_PRESSED}
#else
/* Delete */	{ VK_DELETE, 83, 0}
#endif
};

const ConsoleDebugger::KEY_MATRIX ConsoleDebugger::FunctionToKeyArray[] = {
/* Cursor Up */	    {VK_UP,      72,	0},
/* Cursor Down */   {VK_DOWN,    80,	0},
/* Cursor Right */  {VK_RIGHT,   77,	0},
/* Cursor Left */   {VK_LEFT,    75,	0},
/* End */	    {VK_END,     79,	0},
/* Home */	    {VK_HOME,    71,	0},
/* PageUp */	    {VK_PRIOR,   73,	0},
/* PageDown */	    {VK_NEXT,    81,	0},
/* Insert */	    {VK_INSERT,  82,	0},
/* Delete */	    {VK_DELETE,  83,	0},
/* Select */	    {VK_SELECT,   0,	0},
/* F1 */	    {VK_F1,      59,	0},
/* F2 */	    {VK_F2,      60,	0},
/* F3 */	    {VK_F3,      61,	0},
/* F4 */	    {VK_F4,      62,	0},
/* F5 */	    {VK_F5,      63,	0},
/* F6 */	    {VK_F6,      64,	0},
/* F7 */	    {VK_F7,      65,	0},
/* F8 */	    {VK_F8,      66,	0},
/* F9 */	    {VK_F9,      67,	0},
/* F10 */	    {VK_F10,     68,	0},
/* F11 */	    {VK_F11,     87,	0},
/* F12 */	    {VK_F12,     88,	0},
/* F13 */	    {VK_F13,      0,	0},
/* F14 */	    {VK_F14,      0,	0},
/* F15 */	    {VK_F15,      0,	0},
/* F16 */	    {VK_F16,      0,	0},
/* F17 */	    {VK_F17,      0,	0},
/* F18 */	    {VK_F18,      0,	0},
/* F19 */	    {VK_F19,      0,	0},
/* F20 */	    {VK_F20,      0,	0}
};

const DWORD ConsoleDebugger::KEY_UP	= 0;
const DWORD ConsoleDebugger::KEY_DOWN	= 1;
const DWORD ConsoleDebugger::KEY_RIGHT	= 2;
const DWORD ConsoleDebugger::KEY_LEFT	= 3;
const DWORD ConsoleDebugger::KEY_END	= 4;
const DWORD ConsoleDebugger::KEY_HOME	= 5;
const DWORD ConsoleDebugger::KEY_PAGEUP	= 6;
const DWORD ConsoleDebugger::KEY_PAGEDOWN= 7;
const DWORD ConsoleDebugger::KEY_INSERT	= 8;
const DWORD ConsoleDebugger::KEY_DELETE	= 9;
const DWORD ConsoleDebugger::KEY_SELECT	= 10;
const DWORD ConsoleDebugger::KEY_F1	= 11;
const DWORD ConsoleDebugger::KEY_F2	= 12;
const DWORD ConsoleDebugger::KEY_F3	= 13;
const DWORD ConsoleDebugger::KEY_F4	= 14;
const DWORD ConsoleDebugger::KEY_F5	= 15;
const DWORD ConsoleDebugger::KEY_F6	= 16;
const DWORD ConsoleDebugger::KEY_F7	= 17;
const DWORD ConsoleDebugger::KEY_F8	= 18;
const DWORD ConsoleDebugger::KEY_F9	= 19;
const DWORD ConsoleDebugger::KEY_F10	= 20;
const DWORD ConsoleDebugger::KEY_F11	= 21;
const DWORD ConsoleDebugger::KEY_F12	= 22;
const DWORD ConsoleDebugger::KEY_F13	= 23;
const DWORD ConsoleDebugger::KEY_F14	= 24;
const DWORD ConsoleDebugger::KEY_F15	= 25;
const DWORD ConsoleDebugger::KEY_F16	= 26;
const DWORD ConsoleDebugger::KEY_F17	= 27;
const DWORD ConsoleDebugger::KEY_F18	= 28;
const DWORD ConsoleDebugger::KEY_F19	= 29;
const DWORD ConsoleDebugger::KEY_F20	= 30;
const DWORD ConsoleDebugger::WIN_RESIZE	= 31;

const ConsoleDebugger::FUNCTION_KEY ConsoleDebugger::VtFunctionKeys[] = {
    {"OP",	KEY_F1},
    {"OQ",	KEY_F2},
    {"OR",	KEY_F3},
    {"OS",	KEY_F4},
    {"[A",	KEY_UP},
    {"[B",	KEY_DOWN},
    {"[C",	KEY_RIGHT},
    {"[D",	KEY_LEFT},
    {"[F",	KEY_END},
    {"[H",	KEY_HOME},
    {"[2~",	KEY_INSERT},
    {"[3~",	KEY_DELETE},
    {"[4~",	KEY_SELECT},
    {"[5~",	KEY_PAGEUP},
    {"[6~",	KEY_PAGEDOWN},
    {"[11~",	KEY_F1},
    {"[12~",	KEY_F2},
    {"[13~",	KEY_F3},
    {"[14~",	KEY_F4},
    {"[15~",	KEY_F5},
    {"[17~",	KEY_F6},
    {"[18~",	KEY_F7},
    {"[19~",	KEY_F8},
    {"[20~",	KEY_F9},
    {"[21~",	KEY_F10},
    {"[23~",	KEY_F11},
    {"[24~",	KEY_F12},
    {"[25~",	KEY_F13},
    {"[26~",	KEY_F14},
    {"[28~",	KEY_F15},
    {"[29~",	KEY_F16},
    {"[31~",	KEY_F17},
    {"[32~",	KEY_F18},
    {"[33~",	KEY_F19},
    {"[34~",	KEY_F20},
    {"[39~",	WIN_RESIZE},
    {0L,	0}
};

DWORD
ConsoleDebugger::MapCharToIRs (CHAR c)
{
#ifndef IPC_MAXRECORDS // Only used if we send single key events
    UCHAR lc;
    DWORD mods, result;
    IPCMsg msg;

    /* strip off the upper 127 */
    lc = (UCHAR) (c & 0x7f);
    mods = AsciiToKeyArray[lc].dwControlKeyState;

    msg.type = IRECORD;
    msg.irecord.EventType = KEY_EVENT;

#if 0
    if (mods & RIGHT_CTRL_PRESSED) {
	/* First, generate a control key press */
	msg.irecord.Event.KeyEvent.bKeyDown = TRUE;
	msg.irecord.Event.KeyEvent.wRepeatCount = 1;
	msg.irecord.Event.KeyEvent.wVirtualKeyCode =
		ModifierKeyArray[KEY_CONTROL].wVirtualKeyCode;
	msg.irecord.Event.KeyEvent.wVirtualScanCode =
		ModifierKeyArray[KEY_CONTROL].wVirtualScanCode;
	msg.irecord.Event.KeyEvent.uChar.AsciiChar = 0;
	msg.irecord.Event.KeyEvent.dwControlKeyState = RIGHT_CTRL_PRESSED;
	result = Write(&msg);
	if (result != NO_ERROR) return result;
    }
    if (mods & SHIFT_PRESSED) {
	/* First, generate a control key press */
	msg.irecord.Event.KeyEvent.bKeyDown = TRUE;
	msg.irecord.Event.KeyEvent.wVirtualKeyCode =
		ModifierKeyArray[KEY_SHIFT].wVirtualKeyCode;
	msg.irecord.Event.KeyEvent.wVirtualScanCode =
		ModifierKeyArray[KEY_SHIFT].wVirtualScanCode;
	msg.irecord.Event.KeyEvent.uChar.AsciiChar = 0;
	msg.irecord.Event.KeyEvent.dwControlKeyState = mods;
	result = Write(&msg);
	if (result != NO_ERROR) return result;
    }
#endif

    /* keydown first. */
    msg.irecord.Event.KeyEvent.bKeyDown = TRUE;
    msg.irecord.Event.KeyEvent.wRepeatCount = 1;
    msg.irecord.Event.KeyEvent.wVirtualKeyCode =
	    AsciiToKeyArray[lc].wVirtualKeyCode;
    msg.irecord.Event.KeyEvent.wVirtualScanCode =
	    AsciiToKeyArray[lc].wVirtualScanCode;
    msg.irecord.Event.KeyEvent.dwControlKeyState =
	    AsciiToKeyArray[lc].dwControlKeyState;
    msg.irecord.Event.KeyEvent.uChar.AsciiChar = c;
    result = Write(&msg);
    if (result != NO_ERROR) return result;

    /* now keyup. */
    msg.irecord.Event.KeyEvent.bKeyDown = FALSE;
    msg.irecord.Event.KeyEvent.wRepeatCount = 1;
    msg.irecord.Event.KeyEvent.wVirtualKeyCode =
	    AsciiToKeyArray[lc].wVirtualKeyCode;
    msg.irecord.Event.KeyEvent.wVirtualScanCode =
	    AsciiToKeyArray[lc].wVirtualScanCode;
    msg.irecord.Event.KeyEvent.dwControlKeyState =
	    AsciiToKeyArray[lc].dwControlKeyState;
    msg.irecord.Event.KeyEvent.uChar.AsciiChar = c;
    result = Write(&msg);
    if (result != NO_ERROR) return result;

#if 0
    if (mods & SHIFT_PRESSED) {
	/* First, generate a control key press */
	msg.irecord.Event.KeyEvent.bKeyDown = FALSE;
	msg.irecord.Event.KeyEvent.wVirtualKeyCode =
		ModifierKeyArray[KEY_SHIFT].wVirtualKeyCode;
	msg.irecord.Event.KeyEvent.wVirtualScanCode =
		ModifierKeyArray[KEY_SHIFT].wVirtualScanCode;
	msg.irecord.Event.KeyEvent.uChar.AsciiChar = 0;
	msg.irecord.Event.KeyEvent.dwControlKeyState = mods & ~SHIFT_PRESSED;
	result = Write(&msg);
	if (result != NO_ERROR) return result;
    }
    if (mods & RIGHT_CTRL_PRESSED) {
	/* First, generate a control key press */
	msg.irecord.Event.KeyEvent.bKeyDown = FALSE;
	msg.irecord.Event.KeyEvent.wVirtualKeyCode =
		ModifierKeyArray[KEY_CONTROL].wVirtualKeyCode;
	msg.irecord.Event.KeyEvent.wVirtualScanCode =
		ModifierKeyArray[KEY_CONTROL].wVirtualScanCode;
	msg.irecord.Event.KeyEvent.uChar.AsciiChar = 0;
	msg.irecord.Event.KeyEvent.dwControlKeyState = 0;
	result = Write(&msg);
	if (result != NO_ERROR) return result;
    }
#endif
#endif // IPC_MAXRECORDS
    return NO_ERROR;
}

#ifndef IPC_MAXRECORDS // not used
DWORD
ConsoleDebugger::MapFKeyToIRs(DWORD fk)
{
    IPCMsg msg;
    DWORD result;

    msg.type = IRECORD;
    msg.irecord.EventType = KEY_EVENT;

    /* keydown first. */
    msg.irecord.Event.KeyEvent.bKeyDown = TRUE;
    msg.irecord.Event.KeyEvent.wRepeatCount = 1;
    msg.irecord.Event.KeyEvent.wVirtualKeyCode =
	    FunctionToKeyArray[fk].wVirtualKeyCode;
    msg.irecord.Event.KeyEvent.wVirtualScanCode =
	    FunctionToKeyArray[fk].wVirtualScanCode;
    msg.irecord.Event.KeyEvent.dwControlKeyState =
	    FunctionToKeyArray[fk].dwControlKeyState;
    msg.irecord.Event.KeyEvent.uChar.AsciiChar = 0;
    result = Write(&msg);
    if (result != NO_ERROR) return result;

    /* now keyup. */
    msg.irecord.Event.KeyEvent.bKeyDown = FALSE;
    msg.irecord.Event.KeyEvent.wRepeatCount = 1;
    msg.irecord.Event.KeyEvent.wVirtualKeyCode =
	    FunctionToKeyArray[fk].wVirtualKeyCode;
    msg.irecord.Event.KeyEvent.wVirtualScanCode =
	    FunctionToKeyArray[fk].wVirtualScanCode;
    msg.irecord.Event.KeyEvent.dwControlKeyState =
	    FunctionToKeyArray[fk].dwControlKeyState;
    msg.irecord.Event.KeyEvent.uChar.AsciiChar = 0;
    result = Write(&msg);
    if (result != NO_ERROR) return result;

    return NO_ERROR;
}
#endif

/*
 *----------------------------------------------------------------------
 *
 * FindEscapeKey --
 *
 *	Search for a matching escape key sequence
 *
 * Results:
 *	The matching key if found, -1 if not found, -2 if a partial match
 *
 *----------------------------------------------------------------------
 */

int
ConsoleDebugger::FindEscapeKey(LPCSTR buf, SIZE_T buflen)
{
    DWORD len;
    int i;

    for (i = 0; VtFunctionKeys[i].sequence; i++) {
	len = strlen(VtFunctionKeys[i].sequence);
	if (len == buflen) {
	    if (strncmp(VtFunctionKeys[i].sequence, buf, buflen) == 0) {
		return VtFunctionKeys[i].keyval;
	    }
	} else {
	    if (strncmp(VtFunctionKeys[i].sequence, buf, buflen) == 0) {
		/* Partial match */
		return -2;
	    }
	}
    }
    return -1;
}

int
ConsoleDebugger::Write (LPCSTR buffer, SIZE_T length, LPDWORD err)
{
#ifndef IPC_MAXRECORDS
    SIZE_T i;
    DWORD errorCode;
    int key;

    *err = NO_ERROR;

    for (i = 0; i < length; i++) {
	char c;

	c = *(buffer+i);
	// scan looking for '\033', send to MapFKeyToIRs instead.
	if (0 && c == '\033') {    // XXX: this needs work!  Bypass branch
	    key = FindEscapeKey(buffer+i, length-i);
	    if (key >= 0) {
		if ((errorCode = MapFKeyToIRs(key)) != NO_ERROR) {
		    *err = errorCode;
		    return -1;
		}
	    }
	} else {
	    if ((errorCode = MapCharToIRs(*(buffer+i))) != NO_ERROR) {
		*err = errorCode;
		return -1;
	    }
	}
    }
#else
    SIZE_T i, j;
    CHAR c;
    UCHAR lc;
    DWORD result;
    IPCMsg msg;

    *err = NO_ERROR;

    msg.type = IRECORD;

    for (i = 0, j = 0; i < length; i++) {
	c = buffer[i];
	/* strip off the upper 127 */
	lc = (UCHAR) (c & 0x7f);

	/* keydown first. */
	msg.irecord[j].EventType = KEY_EVENT;
	msg.irecord[j].Event.KeyEvent.bKeyDown = TRUE;
	msg.irecord[j].Event.KeyEvent.wRepeatCount = 1;
	msg.irecord[j].Event.KeyEvent.wVirtualKeyCode =
	    AsciiToKeyArray[lc].wVirtualKeyCode;
	msg.irecord[j].Event.KeyEvent.wVirtualScanCode =
	    AsciiToKeyArray[lc].wVirtualScanCode;
	msg.irecord[j].Event.KeyEvent.dwControlKeyState =
	    AsciiToKeyArray[lc].dwControlKeyState;
	msg.irecord[j].Event.KeyEvent.uChar.AsciiChar = c;
	j++;

	/* now keyup. */
	msg.irecord[j].EventType = KEY_EVENT;
	msg.irecord[j].Event.KeyEvent.bKeyDown = FALSE;
	msg.irecord[j].Event.KeyEvent.wRepeatCount = 1;
	msg.irecord[j].Event.KeyEvent.wVirtualKeyCode =
	    AsciiToKeyArray[lc].wVirtualKeyCode;
	msg.irecord[j].Event.KeyEvent.wVirtualScanCode =
	    AsciiToKeyArray[lc].wVirtualScanCode;
	msg.irecord[j].Event.KeyEvent.dwControlKeyState =
	    AsciiToKeyArray[lc].dwControlKeyState;
	msg.irecord[j].Event.KeyEvent.uChar.AsciiChar = c;
	j++;

	if ((j >= IPC_MAXRECORDS-1) || (i == length-1)) {
	    /*
	     * Write out the input records to the console.
	     */
	    msg.event = j;
	    result = Write(&msg);
	    if (result != NO_ERROR) {
		*err = result;
		return -1;
	    }
	    j = 0;
	}
    }
#endif

    return length;   // we always can write the whole thing.
}

