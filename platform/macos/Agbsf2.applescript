-- SPDX-License-Identifier: MIT

-- Drop the ROMs (.gba) or GSF rips (.gsflib, .minigsf) of GBA games on this
-- app to convert their music to MIDI files and SoundFonts. Each file's results
-- go in a folder beside it, with the same name.
--
-- The command-line tool is inside the app, in Contents/Resources. CMake appends xsflib's shared handlers
-- (showResults, lastPathComponent, lastLine and joinLines) to this script when it builds the app.

-- The command-line tool's file name, and the app's name for its dialogs.
property toolName : "agbsf2"
property appTitle : "Agbsf2"

on run
	set theFiles to choose file with prompt "Choose GBA ROMs or GSF rips to convert:" with multiple selections allowed
	showResults(convertFiles(theFiles, toolPath()), appTitle)
end run

on open theFiles
	showResults(convertFiles(theFiles, toolPath()), appTitle)
end open

on toolPath()
	return POSIX path of (path to resource toolName)
end toolPath

-- Runs the tool once for all files so mini-GSFs sharing a library are converted once.
-- Returns {report, output folders}, with one report line per input file.
on convertFiles(theFiles, tool)
	set commandLine to quoted form of tool
	set fileNames to {}
	repeat with f in theFiles
		set p to POSIX path of f
		set commandLine to commandLine & " " & quoted form of p
		set end of fileNames to lastPathComponent(p)
	end repeat

	try
		set toolOutput to do shell script commandLine & " 2>&1"
	on error errText
		-- Conversion failed for a file or song. stderr was redirected, so errText contains the tool's output.
		set toolOutput to errText
	end try

	-- The tool reports on the files in order, and its first line about each one starts with the
	-- file's name. After that, "converted ..." reports the result, "output: ..." names its
	-- folder, and another line starting with its name is an error that stopped its conversion.
	set reportLines to {}
	set outputFolders to {}
	set fileIndex to 0
	repeat with ln in paragraphs of toolOutput
		set ln to ln as text
		if fileIndex < (count of fileNames) and ln starts with ((item (fileIndex + 1) of fileNames) & ": ") then
			set fileIndex to fileIndex + 1
			set end of reportLines to ln
		else if fileIndex > 0 and ln starts with ((item fileIndex of fileNames) & ": ") then
			set item -1 of reportLines to ln
		else if fileIndex > 0 and ln starts with "converted " then
			set item -1 of reportLines to (item fileIndex of fileNames) & ": " & ln
		else if ln starts with "output: " then
			set outDir to text 9 thru -1 of ln
			if outputFolders does not contain outDir then set end of outputFolders to outDir
		end if
	end repeat

	-- If no file was reported, the tool failed before conversion. Its last line gives the error.
	if reportLines is {} then set end of reportLines to lastLine(toolOutput)
	return {joinLines(reportLines), outputFolders}
end convertFiles
