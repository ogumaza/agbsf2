-- SPDX-License-Identifier: MIT

-- Tests the droplet's conversion report and its list of output folders. Calls convertFiles with
-- six inputs. One of them is a folder. fake_tool.sh stands in for the program. Finder drops and
-- dialogs aren't tested.
--
--     osascript droplet_test.applescript <the app's Contents/Resources/Scripts/main.scpt>
--               <fake_tool.sh>

on run argv
	set droplet to load script ((POSIX file (item 1 of argv)) as alias)
	set tool to item 2 of argv

	-- The Finder hands the droplet aliases, and an alias needs a file that exists.
	set tempFolder to do shell script "mktemp -d"
	set theFiles to {}
	-- A folder's POSIX path ends in a slash. The program names the folder without it.
	repeat with n in {"good.gba", "folder", "partial.gba", "bad.gba", "set-01.minigsf", "set-02.minigsf"}
		set p to tempFolder & "/" & (n as text)
		if (n as text) is "folder" then
			do shell script "mkdir " & quoted form of p
		else
			do shell script "touch " & quoted form of p
		end if
		set end of theFiles to (POSIX file p) as alias
	end repeat

	-- The fake tool names the output folders after the paths the droplet passes it.
	set fileFolder to do shell script "dirname " & quoted form of (POSIX path of (item 1 of theFiles))
	try
		set {reportText, outputFolders} to droplet's convertFiles(theFiles, tool)
	on error errText number errNumber
		do shell script "rm -rf " & quoted form of tempFolder
		error errText number errNumber
	end try
	do shell script "rm -rf " & quoted form of tempFolder

	set expected to "good.gba: converted 2 songs" & return
	set expected to expected & "folder: it's a folder, not a ROM or GSF file" & return
	set expected to expected & "partial.gba: converted 1 song, 1 failed" & return
	set expected to expected & "bad.gba: no Konami, Rare, Quintet, Nintendo R&D1, Nintendo R&D2, AlphaDream, "
	set expected to expected & "Brownie Brown, Ubisoft Milan, Krawall or MP2K sound driver found: "
	set expected to expected & "this game's music uses another engine, or a driver version that agbsf2 "
	set expected to expected & "doesn't support" & return
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
