- The installer can make ring0 the Task Manager: Ctrl+Shift+Esc, Ctrl+Alt+Del and the taskbar menu open ring0. On by default; untick "Use ring0 as the Task Manager" to keep the stock one, or install unattended with `/O:replace_taskmgr=off`.
- This sets the Image File Execution Options `Debugger` value on `taskmgr.exe`, the same mechanism Process Explorer uses. EDR and antivirus products watch that key because malware abuses it, so yours may raise an alert or block the change. Uninstalling ring0 restores the stock Task Manager.

Requires Windows 11 24H2 or later with sudo for Windows enabled in Inline mode (`sudo config --enable normal`). The installer checks both.
