-- SPDX-License-Identifier: MIT

-- Tests the droplet's conversion report and output folder list. Calls convertFiles with five
-- inputs, using fake_tool.sh in place of the program. Does not test Finder drops or dialogs.
--
--     osascript droplet_test.applescript <the app's Contents/Resources/Scripts/main.scpt>
--               <fake_tool.sh>

on run argv
	set droplet to load script ((POSIX file (item 1 of argv)) as alias)
	set tool to item 2 of argv

	-- The Finder hands the droplet aliases, and an alias needs a file that exists.
	set tempFolder to do shell script "mktemp -d"
	set theFiles to {}
	repeat with n in {"good.gba", "partial.gba", "bad.gba", "set-01.minigsf", "set-02.minigsf"}
		set p to tempFolder & "/" & (n as text)
		do shell script "touch " & quoted form of p
		set end of theFiles to (POSIX file p) as alias
	end repeat

	-- The fake tool names the output folders after the paths the droplet passes it.
	set fileFolder to text 1 thru -10 of (POSIX path of (item 1 of theFiles))
	try
		set {reportText, outputFolders} to droplet's convertFiles(theFiles, tool)
	on error errText number errNumber
		do shell script "rm -rf " & quoted form of tempFolder
		error errText number errNumber
	end try
	do shell script "rm -rf " & quoted form of tempFolder

	set expected to "good.gba: converted 2 songs" & return
	set expected to expected & "partial.gba: converted 1 song, 1 failed" & return
	set expected to expected & "bad.gba: no Konami, Rare, Quintet, Nintendo R&D2, Brownie Brown, Ubisoft Milan, Krawall or MP2K sound driver found: "
	set expected to expected & "this game's music uses another engine, or a driver version that agbsf2 doesn't support" & return
	set expected to expected & "set-01.minigsf: converted 2 songs" & return
	set expected to expected & "set-02.minigsf: same music as set.gsflib, already converted"
	considering case
		if reportText is not expected then
			error "The droplet's report is wrong:" & return & reportText
		end if
		if outputFolders is not {fileFolder & "/good", fileFolder & "/partial", fileFolder & "/set"} then
			set AppleScript's text item delimiters to ", "
			error "The droplet's output folders are wrong: " & (outputFolders as text)
		end if
	end considering

	return "The droplet's report and output folders are right"
end run
