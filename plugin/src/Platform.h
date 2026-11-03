#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

// What differs between Windows and the other systems: where files live, and opening files by
// UTF-8 path (Windows needs wide-character calls for names outside the ANSI code page, such as
// a user folder with an umlaut). All paths here are UTF-8.

// Where the import tool and the plugin keep their shared files:
//   Windows  %APPDATA%\Insta360Stitch
//   macOS    ~/Library/Application Support/Insta360Stitch
//   other    ~/.local/share/Insta360Stitch
std::string SupportDir();

// Folder for log files: %APPDATA%\Insta360Stitch\Logs on Windows, ~/Library/Logs on macOS.
std::string LogDir();

// Folder the running plugin binary is in.
std::string ModuleDir();

// p_A + separator + p_B.
std::string JoinPath(const std::string& p_A, const std::string& p_B);

FILE* OpenFile(const std::string& p_Path, const char* p_Mode);

// Whole file as bytes; false if it cannot be read.
bool ReadWholeFile(const std::string& p_Path, std::string& p_Out);

// Modification time and size, to notice a rewritten file; false if it does not exist.
bool FileStamp(const std::string& p_Path, int64_t& p_Modified, int64_t& p_Size);

bool FileExists(const std::string& p_Path);

#ifdef _WIN32
std::wstring Widen(const std::string& p_Utf8);
std::string Narrow(const std::wstring& p_Wide);
#endif

// "CPU <name>, <n> threads; GPU <name>", for the log.
std::string SystemDescription();

// Creates a folder and the folders it is in; true if it exists afterwards.
bool MakeDirectories(const std::string& p_Path);
