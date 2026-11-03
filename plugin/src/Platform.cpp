#include "Platform.h"

#include <cstdlib>
#include <sys/stat.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#else
#include <dlfcn.h>
#endif

#ifdef _WIN32
std::wstring Widen(const std::string& p_Utf8)
{
    if (p_Utf8.empty())
    {
        return std::wstring();
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, p_Utf8.data(), int(p_Utf8.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, p_Utf8.data(), int(p_Utf8.size()), &out[0], n);
    return out;
}

std::string Narrow(const std::wstring& p_Wide)
{
    if (p_Wide.empty())
    {
        return std::string();
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, p_Wide.data(), int(p_Wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, p_Wide.data(), int(p_Wide.size()), &out[0], n, nullptr, nullptr);
    return out;
}

static std::string KnownFolder(REFKNOWNFOLDERID p_Id)
{
    PWSTR path = nullptr;
    std::string out;
    if (SHGetKnownFolderPath(p_Id, 0, nullptr, &path) == S_OK && path)
    {
        out = Narrow(path);
    }
    CoTaskMemFree(path);
    return out;
}
#endif

std::string JoinPath(const std::string& p_A, const std::string& p_B)
{
    if (p_A.empty())
    {
        return p_B;
    }
    const char last = p_A.back();
    if (last == '/' || last == '\\')
    {
        return p_A + p_B;
    }
#ifdef _WIN32
    return p_A + "\\" + p_B;
#else
    return p_A + "/" + p_B;
#endif
}

static std::string Home()
{
    const char* home = getenv("HOME");
    return home ? home : "";
}

std::string SupportDir()
{
#ifdef _WIN32
    // Roaming application data, which is what Python's os.environ["APPDATA"] gives the import tool.
    std::string base = KnownFolder(FOLDERID_RoamingAppData);
    return base.empty() ? std::string() : JoinPath(base, "Insta360Stitch");
#elif defined(__APPLE__)
    return Home().empty() ? std::string() : Home() + "/Library/Application Support/Insta360Stitch";
#else
    return Home().empty() ? std::string() : Home() + "/.local/share/Insta360Stitch";
#endif
}

std::string LogDir()
{
#ifdef _WIN32
    const std::string support = SupportDir();
    return support.empty() ? std::string() : JoinPath(support, "Logs");
#elif defined(__APPLE__)
    return Home().empty() ? std::string("/tmp") : Home() + "/Library/Logs";
#else
    return Home().empty() ? std::string("/tmp") : Home() + "/.local/share/Insta360Stitch/Logs";
#endif
}

std::string ModuleDir()
{
#ifdef _WIN32
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ModuleDir), &module))
    {
        return "";
    }
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(module, &path[0], DWORD(path.size()));
    if (n == 0 || n >= path.size())
    {
        return "";
    }
    path.resize(n);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::string() : Narrow(path.substr(0, slash));
#else
    Dl_info info;
    if (!dladdr(reinterpret_cast<void*>(&ModuleDir), &info) || !info.dli_fname)
    {
        return "";
    }
    std::string path = info.dli_fname;
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
#endif
}

FILE* OpenFile(const std::string& p_Path, const char* p_Mode)
{
#ifdef _WIN32
    return _wfopen(Widen(p_Path).c_str(), Widen(p_Mode).c_str());
#else
    return fopen(p_Path.c_str(), p_Mode);
#endif
}

bool ReadWholeFile(const std::string& p_Path, std::string& p_Out)
{
    FILE* f = OpenFile(p_Path, "rb");
    if (!f)
    {
        return false;
    }
    p_Out.clear();
    char buffer[65536];
    size_t got;
    while ((got = fread(buffer, 1, sizeof(buffer), f)) > 0)
    {
        p_Out.append(buffer, got);
    }
    const bool ok = !ferror(f);
    fclose(f);
    return ok;
}

bool FileStamp(const std::string& p_Path, int64_t& p_Modified, int64_t& p_Size)
{
#ifdef _WIN32
    struct _stat64 info;
    if (_wstat64(Widen(p_Path).c_str(), &info) != 0)
    {
        return false;
    }
#else
    struct stat info;
    if (stat(p_Path.c_str(), &info) != 0)
    {
        return false;
    }
#endif
    p_Modified = int64_t(info.st_mtime);
    p_Size = int64_t(info.st_size);
    return true;
}

bool FileExists(const std::string& p_Path)
{
    int64_t modified, size;
    return FileStamp(p_Path, modified, size);
}

bool MakeDirectories(const std::string& p_Path)
{
    if (p_Path.empty())
    {
        return false;
    }
    if (FileExists(p_Path))
    {
        return true;
    }
    const size_t slash = p_Path.find_last_of("/\\");
    if (slash != std::string::npos && slash > 0)
    {
        const std::string parent = p_Path.substr(0, slash);
        // Stop at a drive root such as "C:".
        if (!(parent.size() == 2 && parent[1] == ':'))
        {
            MakeDirectories(parent);
        }
    }
#ifdef _WIN32
    CreateDirectoryW(Widen(p_Path).c_str(), nullptr);
#else
    mkdir(p_Path.c_str(), 0755);
#endif
    return FileExists(p_Path);
}
