# Scripts, workspaces and command shortcuts

Search **Manage Automation**, or open any Library manager from **Settings > Library**. The Scripts, Workspaces and Command Shortcuts tabs save to `settings.json`. Changes become available without restarting.

## Scripts

Add a name, search keyword and absolute path to an existing `.ps1` file. Selecting the result runs the file using Windows PowerShell with `-NoProfile -File`, in the script's containing directory. FeatherCast does not bypass execution policy or elevate the script. Only add scripts you understand and trust; scripts have your Windows account's permissions. A successful launch confirms that PowerShell started, not that the script finished successfully. PowerShell displays execution or policy errors in its own window.

## Workspaces

Add a name, keyword and up to 16 absolute paths or `http://` / `https://` addresses, one per line. Launching opens the configured apps, documents, folders and pages using Windows file associations. Command strings and script files are not accepted as workspace entries. Use the Scripts tab for PowerShell.

Every target is attempted. If any target cannot be opened, the launcher reports a launch failure; already opened targets remain open. Paths and URLs are stored locally as readable configuration. Avoid embedding passwords or tokens in URLs.

## Command shortcuts

Choose a built-in command and a chord containing Ctrl or Alt, such as `Ctrl+Alt+V`. Windows-key shortcuts and bare keys are unavailable in this tab. Existing launcher/capture shortcuts, duplicate assignments and OS/app reservations are rejected; the previous assignments remain active if registration fails.

On a command result, open its actions and choose **Set Global Shortcut** to edit that command directly. Commands keep their normal confirmation behavior. Delete an assignment to release its shortcut. Assignments apply while FeatherCast is running.

## Local search learning

**Settings > Results > Learn Search Choices** is off by default. When enabled, root searches can remember up to 256 query-to-action choices for local ranking. Exact aliases and exact names retain priority. This stores the query in readable local settings; file and scoped queries are excluded. Turning the setting off clears remembered choices. **Reset Ranking** in an item's actions clears its remembered choices and recent usage; **Clear Recents** also clears remembered query choices.
