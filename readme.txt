Old windows port of Expect from around 2003.  It used the DEBUG_PROCESS
flag for CreateProcess() to then set breakpoints on console API calls
and then reroute into Expect.  But to get keypresses back into the hidden
child process, it was nesessary to load a special dll that acted as the
'press-ee'.  It was this forced loading at a breakpoint that caused a DEP
security flag
https://learn.microsoft.com/en-us/windows/win32/memory/data-execution-prevention

Umm, yes, that's exactly what we're doing here and described in the
comments:
https://github.com/davygrvy/expect4win/blob/75b3a4ff285e8ff4c5fbdce56858c5bf8464974c/win/expWinConsoleDebugger.cpp#L662-L677


There must be a better way?  Yes, this.  I will slowly port to Detours
in my free time.
https://github.com/microsoft/Detours/wiki