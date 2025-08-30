/* ----------------------------------------------------------------------------
 * expWinConsoleDebugger.hpp --
 *
 *	Console debugger class declared here.
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
 * RCS: @(#) $Id: expWinConsoleDebugger.hpp,v 1.1.2.31 2003/08/26 20:46:52 davygrvy Exp $
 * ----------------------------------------------------------------------------
 */

#ifndef INC_expWinConsoleDetourer_hpp__
#define INC_expWinConsoleDetourer_hpp__

#include "CMcl.h"
#include "expWinMessage.hpp"
//#include "expWinTrampolineIPC.hpp"

// for the hash table template (argh! I really don't want Tcl deps in this code).
//#include "TclHash.hpp"


// callback type.
class ConsoleDebuggerCallback
{
    friend class ConsoleDebugger;
    virtual void AlertReadable (void) = 0;
};



//  This is our detourer.  We run it in a thread. 
//
class ConsoleDetourer : public CMclThreadHandler
{
public:

    // NOTE: TCHAR is a loose definition and *NOT* the normal
    // usage as found in the Win32 API reference.  TCHAR is
    // runtime selected to be WCHAR (unicode) on NT/2K/XP
    // or CHAR (local specific) on everything else.  See
    // Tcl_WinUtfToTChar in the Tcl docs for details and
    // the WinProcs struct in expWinPort.h
    //
    ConsoleDetourer(
	TCHAR *_cmdline,		// commandline string (in system encoding)
	TCHAR *_env,			// environment block (in system encoding)
	TCHAR *_dir,			// startup directory (in system encoding)
					// These 3 maintain a reference until
					//  _readyUp is signaled.
	int show,			// $exp::nt_debug, shows spawned children.
	LPCSTR _trampPath,		// location on the file system of the trampoline dll
	CMclLinkedList<Message *> &_mQ,	// parent owned linkedlist for returning data stream.
	CMclLinkedList<Message *> &_eQ,	// parent owned linkedlist for returning error stream.
	CMclEvent &_readyUp,		// set when child process is ready (or failed).
	ConsoleDebuggerCallback &_callback
	);

    // copying and passing by copy are not allowed...
    // this prevents confusion of internal object ownership...
    ConsoleDetourer(ConsoleDetourer& rhs);

    // assigning one ConsoleDebugger to another is not allowed,
    // this prevents confusion of internal object ownership...
    ConsoleDetourer& operator= (ConsoleDetourer& rhs);

    ~ConsoleDetourer();

    inline DWORD Status (void) { return status; }	    // retrieves error codes.
    inline DWORD Pid (void) { return pid; }
    inline HANDLE Handle (void) { return hRootProcess; }
    inline HANDLE Console (void) { return hMasterConsole; }
    inline HANDLE ConsoleScreenBuffer (void) { return hCopyScreenBuffer; }
    int Write (LPCSTR buffer, SIZE_T length, LPDWORD err); // send vt100 to the slave console.
    DWORD Write (IPCMsg *msg);			    // send an INPUT_RECORD or ctrlEvent instead.

    void EnterInteract (HANDLE OutConsole);	    // enters interact mode.
    void ExitInteract (void);			    // exits interact mode.

    DWORD fatalException;

private:
    virtual unsigned ThreadHandlerProc(void);

    CMclMailbox *trampIPC;	// IPC transfer mechanism to the trampoline dll.

    bool interacting;

    DWORD status;
    DWORD pid;
    HANDLE hRootProcess;
    HANDLE hMasterConsole;
    HANDLE hCopyScreenBuffer;


    const static DWORD KEY_UP;
    const static DWORD KEY_DOWN;
    const static DWORD KEY_RIGHT;
    const static DWORD KEY_LEFT;
    const static DWORD KEY_END;
    const static DWORD KEY_HOME;
    const static DWORD KEY_PAGEUP;
    const static DWORD KEY_PAGEDOWN;
    const static DWORD KEY_INSERT;
    const static DWORD KEY_DELETE;
    const static DWORD KEY_SELECT;
    const static DWORD KEY_F1;
    const static DWORD KEY_F2;
    const static DWORD KEY_F3;
    const static DWORD KEY_F4;
    const static DWORD KEY_F5;
    const static DWORD KEY_F6;
    const static DWORD KEY_F7;
    const static DWORD KEY_F8;
    const static DWORD KEY_F9;
    const static DWORD KEY_F10;
    const static DWORD KEY_F11;
    const static DWORD KEY_F12;
    const static DWORD KEY_F13;
    const static DWORD KEY_F14;
    const static DWORD KEY_F15;
    const static DWORD KEY_F16;
    const static DWORD KEY_F17;
    const static DWORD KEY_F18;
    const static DWORD KEY_F19;
    const static DWORD KEY_F20;
    const static DWORD WIN_RESIZE;
    const static DWORD KEY_CONTROL;
    const static DWORD KEY_SHIFT;
    const static DWORD KEY_LSHIFT;
    const static DWORD KEY_RSHIFT;
    const static DWORD KEY_ALT;
    
    struct KEY_MATRIX {
	WORD wVirtualKeyCode;
	WORD wVirtualScanCode;
	DWORD dwControlKeyState;
    };
    
    struct FUNCTION_KEY {
	const char *sequence;
	DWORD keyval;
    };

    const static KEY_MATRIX ModifierKeyArray[];
    const static KEY_MATRIX AsciiToKeyArray[];
    const static KEY_MATRIX FunctionToKeyArray[];
    const static FUNCTION_KEY VtFunctionKeys[];

    DWORD MapCharToIRs (CHAR c);
    DWORD MapFKeyToIRs (DWORD fk);
    int FindEscapeKey (LPCSTR buf, SIZE_T buflen);
};

#endif // INC_expWinConsoleDetourer_hpp__
