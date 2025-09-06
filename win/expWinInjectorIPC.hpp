/* ----------------------------------------------------------------------------
 * expWinInjectorIPC.hpp --
 *
 *	CMclMailbox values saved to a common include file to avoid
 *	differences in the constructor calls on either end of the IPC
 *	connection phase.
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
 * RCS: @(#) $Id: expWinInjectorIPC.hpp,v 1.1.2.3 2003/08/26 20:46:52 davygrvy Exp $
 * ----------------------------------------------------------------------------
 */

#ifndef INC_expWinInjectorIPC_hpp__
#define INC_expWinInjectorIPC_hpp__

 // Note to self.  This crosses process bounderies, so always pass by value

enum IPCtoMsgtype {IRECORD, CTRL_EVENT};

#define IPCto_MAXRECORDS 80
#define IPCto_NAME TEXT("ExpectInjector_pid%d")

typedef struct {
    IPCtoMsgtype type;
    DWORD event;     /* This represents irecord length if type == IRECORD */
    INPUT_RECORD irecord[IPCto_MAXRECORDS];
} IPCtoMsg;

#define IPCto_NUMSLOTS 50
#define IPCto_SLOTSIZE sizeof(IPCtoMsg)

#define IPCfrom_NAME TEXT("ExpectDetour_pid%d")

enum IPCfromMsgtype { FUNC_WriteConsoleW, FUNC_WriteConsoleA, C };

typedef struct {
    IPCfromMsgtype type;
    SIZE_T len;
    union {
	WCHAR uniStr[80];
	CHAR ansiStr[80];
    };
} IPCfromMsg;

#define IPCfrom_NUMSLOTS 20
#define IPCfrom_SLOTSIZE sizeof(IPCfromMsg)

#endif
