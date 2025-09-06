/* ----------------------------------------------------------------------------
 * expWinInjectorMain.cpp --
 *
 *	Console event injector DLL that's loaded into the slave's address space
 *	used by the ConsoleDebugger class for "writing" to the slave.
 *
 * ----------------------------------------------------------------------------
 *
 * Written by: Don Libes, libes@cme.nist.gov, NIST, 12/3/90
 *
 * Design and implementation of this program was paid for by U.S. tax
 * dollars.  Therefore it is public domain.  However, the author and NIST
 * would appreciate credit if this program or parts of it are used.
 *
 * Copyright (c) 1997 Mitel Corporation
 *	work by Gordon Chaffee <chaffee@bmrc.berkeley.edu> for the
 *	first WinNT port.
 *
 * Copyright (c) 2001-2002 Telindustrie, LLC
 * Copyright (c) 2003-2005 ActiveState Corporation
 * Copyright (c) 2025 Liquid State Engineering
 *	work by David Gravereaux <davygrvy@pobox.com> for the stubs
 *	enabled extension, scary C++, and later Detours migration in 2025.
 *
 * ----------------------------------------------------------------------------
 * URLs:    https://www.nist.gov/services-resources/software/expect
 *	    http://expect.sf.net/
 *	    https://web.archive.org/web/19980220232311/http://www.bmrc.berkeley.edu/people/chaffee/expectnt.html
 * ----------------------------------------------------------------------------
 * RCS: @(#) $Id: expWinInjectorMain.cpp,v 1.1.2.16 2003/08/26 20:46:52 davygrvy Exp $
 * ----------------------------------------------------------------------------
 */

#include "Mcl/CMcl.h"
#include <strsafe.h>
#include <stdlib.h>
#include "expWinInjectorIPC.hpp"

#ifdef USE_DETOURS
#   include "Detours/detours.h"
#   ifdef _WIN64
#	pragma comment (lib,"detours64.lib")
#   else
#	pragma comment (lib,"detours32.lib")
#   endif
#endif


class Injector : public CMclThreadHandler
{
    CMclMailbox *ConsoleDebuggerIPC;
    HANDLE console;
    CMclEvent *interrupt;
    CMclThread* injectorThread;
#define SYSMSG_CHARS 512
    TCHAR sysMsgSpace[SYSMSG_CHARS];

public:

    Injector() 
	: ConsoleDebuggerIPC(nullptr), sysMsgSpace{NULL}, injectorThread(nullptr)
    {
	// We already know that the exe this is being loaded into runs in the
	// console/posix subsystem and that we created the process with a new console.
	// No error checking is needed.
	//
	console = CreateFile(TEXT("CONIN$"), GENERIC_WRITE, FILE_SHARE_WRITE,
		nullptr, OPEN_EXISTING, 0, nullptr);

	interrupt = new CMclEvent();
	injectorThread = new CMclThread(this);
    }
    
    ~Injector()
    {
	interrupt->Set();
	injectorThread->Wait(INFINITE);
	CloseHandle(console);
    }

private:

    virtual unsigned ThreadHandlerProc(void)
    {
	TCHAR boxName[50];
	DWORD err, dwWritten;
	IPCtoMsg msg;

	StringCbPrintf(
		boxName, sizeof(boxName), IPCto_NAME, GetCurrentProcessId());

	// Create the shared memory IPC transfer mechanism by name
	// (a mailbox).
	ConsoleDebuggerIPC = 
		new CMclMailbox(IPCto_NUMSLOTS, IPCto_SLOTSIZE, boxName);

	// Check status.
	err = ConsoleDebuggerIPC->Status();
	if (err != NO_ERROR && err != ERROR_ALREADY_EXISTS) {
	    // TODO: come up with a better way to return this error to the user
	    OutputDebugString(GetSysMsg(err));
	    delete ConsoleDebuggerIPC;
	    return EXIT_FAILURE;
	}

	OutputDebugString(TEXT("Expect's injector DLL loaded and ready.\n"));

	// loop receiving messages over IPC until signaled to exit.
	while (ConsoleDebuggerIPC->GetAlertable(&msg, interrupt))
	{
	    switch (msg.type) {
	    case CTRL_EVENT:
		// Generate a Ctrl+C or Ctrl+Break to cause the equivalent
		// of a SIGINT into this process.
		GenerateConsoleCtrlEvent(msg.event, 0);
		break;
	    case IRECORD:
		// Stuff it into this console as if it had been entered
		// by the user.
		WriteConsoleInput(console, msg.irecord, msg.event, &dwWritten);
		break;
	    }
	}
	delete interrupt;
	delete ConsoleDebuggerIPC;
	return EXIT_SUCCESS;
    }

    const TCHAR *
    GetSysMsg(DWORD id)
    {
	int chars;

	chars = StringCbPrintf(sysMsgSpace, SYSMSG_CHARS,
		TEXT("Expect's injector DLL could not start IPC: [%u] "), id);
	FormatMessage(
		FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK,
		NULL, id, MAKELANGID(LANG_NEUTRAL,SUBLANG_SYS_DEFAULT),
		sysMsgSpace+chars, (SYSMSG_CHARS-chars), NULL);
	return sysMsgSpace;
    }
};

Injector *inject;
CMclMailbox *fromTrampIPC;

BOOL
WINAPI
MyWriteConsoleA(
    _In_ HANDLE hConsoleOutput,
    _In_reads_(nNumberOfCharsToWrite) LPCSTR lpBuffer,
    _In_ DWORD nNumberOfCharsToWrite,
    _Out_opt_ LPDWORD lpNumberOfCharsWritten,
    _Reserved_ LPVOID lpReserved
)
{
    // Assume with lpNumberOfCharsWritten, being optional, consumes all of
    // it, always.

     BOOL succeeded = WriteConsoleA(hConsoleOutput, lpBuffer, nNumberOfCharsToWrite,
	    lpNumberOfCharsWritten, lpReserved);

    if (succeeded)
    {
	IPCfromMsg* msg = new IPCfromMsg;
	msg->type = FUNC_WriteConsoleA;
	// NOTE: lpBuffer might not be null terminated.
	// TODO: manage this in a for loop when static buffer space is too small
	StringCbCopyNA(msg->ansiStr, sizeof(msg->ansiStr), lpBuffer, nNumberOfCharsToWrite);
	msg->len = nNumberOfCharsToWrite;
	fromTrampIPC->Post(msg);
    }
    return succeeded;
};

BOOL
WINAPI
MyWriteConsoleW(
    _In_ HANDLE hConsoleOutput,
    _In_reads_(nNumberOfCharsToWrite) LPCWSTR lpBuffer,
    _In_ DWORD nNumberOfCharsToWrite,
    _Out_opt_ LPDWORD lpNumberOfCharsWritten,
    _Reserved_ LPVOID lpReserved
)
{
     BOOL succeeded = WriteConsoleW(hConsoleOutput, lpBuffer, nNumberOfCharsToWrite,
	    lpNumberOfCharsWritten, lpReserved);

    if (succeeded)
    {
	IPCfromMsg* msg = new IPCfromMsg;
	msg->type = FUNC_WriteConsoleW;
	// NOTE: lpBuffer might not be null terminated.
	// TODO: manage this in a for loop when static buffer space is too small
	StringCbCopyNW(msg->uniStr, sizeof(msg->uniStr), lpBuffer, nNumberOfCharsToWrite);
	msg->len = nNumberOfCharsToWrite;
	fromTrampIPC->Post(msg);
    }
    return succeeded;
};

BOOL WINAPI
DllMain (HINSTANCE hInst, ULONG ulReason, LPVOID lpReserved)
{
    LONG error;
    TCHAR boxName[50];

    // If preloading through the thunk helper, go away.
    if (DetourIsHelperProcess()) {
	return TRUE;
    }

    switch (ulReason) {
    case DLL_PROCESS_ATTACH:
	DisableThreadLibraryCalls(hInst);
	inject = new Injector();

	StringCbPrintf(
	    boxName, sizeof(boxName), IPCfrom_NAME, GetCurrentProcessId());
	fromTrampIPC = new CMclMailbox(IPCfrom_NUMSLOTS, IPCfrom_SLOTSIZE, boxName);

	DetourRestoreAfterWith();
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach((PVOID*)WriteConsoleA, MyWriteConsoleA);
	DetourAttach((PVOID*)WriteConsoleW, MyWriteConsoleW);
	error = DetourTransactionCommit();
	break;
    case DLL_PROCESS_DETACH:
	delete inject;

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourDetach((PVOID*)WriteConsoleA, MyWriteConsoleA);
	DetourDetach((PVOID*)WriteConsoleW, MyWriteConsoleW);
	error = DetourTransactionCommit();
	break;
    }
    return TRUE;
}
