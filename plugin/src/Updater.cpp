// Checks GitHub for a newer release (Windows: WinHTTP). The macOS original does the same with
// NSURLSession in Updater.mm. Only runs when the user presses "Check for Updates".
#include "Updater.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <memory>
#include <sstream>
#include <vector>

#include "Platform.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winhttp.h>
#endif

namespace
{
// Just enough JSON for GitHub's release description: objects, arrays, strings, the rest skipped.
struct Json
{
    enum Kind { eNull, eString, eObject, eArray, eOther } kind = eNull;
    std::string text;
    std::map<std::string, Json> members;
    std::vector<Json> items;

    const Json& operator[](const std::string& p_Key) const
    {
        static const Json none;
        const auto it = members.find(p_Key);
        return it == members.end() ? none : it->second;
    }
};

class JsonParser
{
public:
    explicit JsonParser(const std::string& p_Text) : m_Text(p_Text) {}

    bool parse(Json& p_Out)
    {
        return value(p_Out, 0);
    }

private:
    void space()
    {
        while (m_Pos < m_Text.size() && isspace((unsigned char)m_Text[m_Pos]))
        {
            ++m_Pos;
        }
    }

    static void Append(std::string& p_Out, unsigned p_Code)
    {
        if (p_Code < 0x80)
        {
            p_Out += char(p_Code);
        }
        else if (p_Code < 0x800)
        {
            p_Out += char(0xC0 | (p_Code >> 6));
            p_Out += char(0x80 | (p_Code & 0x3F));
        }
        else if (p_Code < 0x10000)
        {
            p_Out += char(0xE0 | (p_Code >> 12));
            p_Out += char(0x80 | ((p_Code >> 6) & 0x3F));
            p_Out += char(0x80 | (p_Code & 0x3F));
        }
        else
        {
            p_Out += char(0xF0 | (p_Code >> 18));
            p_Out += char(0x80 | ((p_Code >> 12) & 0x3F));
            p_Out += char(0x80 | ((p_Code >> 6) & 0x3F));
            p_Out += char(0x80 | (p_Code & 0x3F));
        }
    }

    bool string(std::string& p_Out)
    {
        if (m_Pos >= m_Text.size() || m_Text[m_Pos] != '"')
        {
            return false;
        }
        ++m_Pos;
        while (m_Pos < m_Text.size())
        {
            const char c = m_Text[m_Pos++];
            if (c == '"')
            {
                return true;
            }
            if (c != '\\')
            {
                p_Out += c;
                continue;
            }
            if (m_Pos >= m_Text.size())
            {
                return false;
            }
            const char e = m_Text[m_Pos++];
            switch (e)
            {
            case 'n': p_Out += '\n'; break;
            case 't': p_Out += '\t'; break;
            case 'r': p_Out += '\r'; break;
            case 'b': p_Out += '\b'; break;
            case 'f': p_Out += '\f'; break;
            case 'u':
            {
                if (m_Pos + 4 > m_Text.size())
                {
                    return false;
                }
                unsigned code = unsigned(strtoul(m_Text.substr(m_Pos, 4).c_str(), nullptr, 16));
                m_Pos += 4;
                if (code >= 0xD800 && code < 0xDC00 && m_Pos + 6 <= m_Text.size() && m_Text[m_Pos] == '\\' && m_Text[m_Pos + 1] == 'u')
                {
                    const unsigned low = unsigned(strtoul(m_Text.substr(m_Pos + 2, 4).c_str(), nullptr, 16));
                    if (low >= 0xDC00 && low < 0xE000)
                    {
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        m_Pos += 6;
                    }
                }
                Append(p_Out, code);
                break;
            }
            default: p_Out += e; break;
            }
        }
        return false;
    }

    bool value(Json& p_Out, int p_Depth)
    {
        if (p_Depth > 64)
        {
            return false;
        }
        space();
        if (m_Pos >= m_Text.size())
        {
            return false;
        }
        const char c = m_Text[m_Pos];
        if (c == '"')
        {
            p_Out.kind = Json::eString;
            return string(p_Out.text);
        }
        if (c == '{')
        {
            p_Out.kind = Json::eObject;
            ++m_Pos;
            space();
            if (m_Pos < m_Text.size() && m_Text[m_Pos] == '}')
            {
                ++m_Pos;
                return true;
            }
            for (;;)
            {
                space();
                std::string key;
                if (!string(key))
                {
                    return false;
                }
                space();
                if (m_Pos >= m_Text.size() || m_Text[m_Pos] != ':')
                {
                    return false;
                }
                ++m_Pos;
                if (!value(p_Out.members[key], p_Depth + 1))
                {
                    return false;
                }
                space();
                if (m_Pos < m_Text.size() && m_Text[m_Pos] == ',')
                {
                    ++m_Pos;
                    continue;
                }
                if (m_Pos < m_Text.size() && m_Text[m_Pos] == '}')
                {
                    ++m_Pos;
                    return true;
                }
                return false;
            }
        }
        if (c == '[')
        {
            p_Out.kind = Json::eArray;
            ++m_Pos;
            space();
            if (m_Pos < m_Text.size() && m_Text[m_Pos] == ']')
            {
                ++m_Pos;
                return true;
            }
            for (;;)
            {
                p_Out.items.emplace_back();
                if (!value(p_Out.items.back(), p_Depth + 1))
                {
                    return false;
                }
                space();
                if (m_Pos < m_Text.size() && m_Text[m_Pos] == ',')
                {
                    ++m_Pos;
                    continue;
                }
                if (m_Pos < m_Text.size() && m_Text[m_Pos] == ']')
                {
                    ++m_Pos;
                    return true;
                }
                return false;
            }
        }
        // Numbers, true, false, null: skipped.
        p_Out.kind = c == 'n' ? Json::eNull : Json::eOther;
        while (m_Pos < m_Text.size() && m_Text[m_Pos] != ',' && m_Text[m_Pos] != '}' && m_Text[m_Pos] != ']')
        {
            ++m_Pos;
        }
        return true;
    }

    const std::string& m_Text;
    size_t m_Pos = 0;
};

std::string Lower(std::string p_Text)
{
    std::transform(p_Text.begin(), p_Text.end(), p_Text.begin(), [](unsigned char c) { return char(tolower(c)); });
    return p_Text;
}

#ifdef _WIN32
const DWORD kTimeoutMs = 10000;

struct InternetHandle
{
    HINTERNET handle = nullptr;
    ~InternetHandle()
    {
        if (handle)
        {
            WinHttpCloseHandle(handle);
        }
    }
};

// GET p_Url, following redirects (GitHub's downloads redirect to another host).
bool Fetch(const std::string& p_Url, std::string& p_Body, long& p_Status, std::string& p_Error)
{
    std::wstring url = Widen(p_Url);
    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {}, path[4096] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 4096;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
    {
        p_Error = "bad address";
        return false;
    }
    InternetHandle session, connection, request;
    session.handle = WinHttpOpen(L"Insta360Stitch-plugin", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.handle)
    {
        // Windows before 8.1 has no automatic proxy setting.
        session.handle = WinHttpOpen(L"Insta360Stitch-plugin", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session.handle)
    {
        p_Error = "no internet access";
        return false;
    }
    WinHttpSetTimeouts(session.handle, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
    connection.handle = WinHttpConnect(session.handle, host, parts.nPort, 0);
    if (!connection.handle)
    {
        p_Error = "cannot reach GitHub";
        return false;
    }
    request.handle = WinHttpOpenRequest(connection.handle, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!request.handle)
    {
        p_Error = "cannot reach GitHub";
        return false;
    }
    const wchar_t* headers = L"Accept: application/vnd.github+json\r\n";
    if (!WinHttpSendRequest(request.handle, headers, DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.handle, nullptr))
    {
        p_Error = GetLastError() == ERROR_WINHTTP_TIMEOUT ? "no answer from GitHub" : "cannot reach GitHub";
        return false;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    p_Status = long(status);
    p_Body.clear();
    for (;;)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.handle, &available))
        {
            p_Error = "the download was interrupted";
            return false;
        }
        if (available == 0)
        {
            break;
        }
        std::string chunk(available, '\0');
        DWORD got = 0;
        if (!WinHttpReadData(request.handle, &chunk[0], available, &got))
        {
            p_Error = "the download was interrupted";
            return false;
        }
        p_Body.append(chunk.data(), got);
    }
    return true;
}
#endif
}

ReleaseInfo FetchLatestRelease(const std::string& p_Repo)
{
    ReleaseInfo info;
#ifdef _WIN32
    std::string body;
    long status = 0;
    if (!Fetch("https://api.github.com/repos/" + p_Repo + "/releases/latest", body, status, info.error))
    {
        return info;
    }
    if (status == 404)
    {
        info.error = "no release has been published yet";
        return info;
    }
    if (status != 200)
    {
        info.error = "GitHub answered with status " + std::to_string(status);
        return info;
    }
    Json json;
    if (!JsonParser(body).parse(json) || json.kind != Json::eObject || json["tag_name"].kind != Json::eString)
    {
        info.error = "GitHub's answer could not be read";
        return info;
    }
    std::string tag = json["tag_name"].text;
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V'))
    {
        tag.erase(0, 1);
    }
    info.version = tag;
    info.notes = json["body"].text;
    info.pageUrl = json["html_url"].text;
    // A Windows installer among the release's files: an .exe with "win" in its name.
    for (const Json& asset : json["assets"].items)
    {
        const std::string name = Lower(asset["name"].text);
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0 && name.find("win") != std::string::npos &&
            asset["browser_download_url"].kind == Json::eString)
        {
            info.pkgUrl = asset["browser_download_url"].text;
            break;
        }
    }
    info.ok = true;
#else
    (void)p_Repo;
    info.error = "update checks are only built for Windows and macOS";
#endif
    return info;
}

int CompareVersions(const std::string& p_A, const std::string& p_B)
{
    auto parts = [](const std::string& v) {
        std::vector<long> out;
        std::stringstream ss(v);
        std::string piece;
        while (std::getline(ss, piece, '.'))
        {
            out.push_back(strtol(piece.c_str(), nullptr, 10));
        }
        return out;
    };
    const std::vector<long> a = parts(p_A), b = parts(p_B);
    for (size_t i = 0; i < std::max(a.size(), b.size()); ++i)
    {
        const long x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x != y)
        {
            return x < y ? -1 : 1;
        }
    }
    return 0;
}

std::string DownloadToDownloads(const std::string& p_Url, std::string& p_Error)
{
#ifdef _WIN32
    std::string body;
    long status = 0;
    if (!Fetch(p_Url, body, status, p_Error) || status != 200)
    {
        if (p_Error.empty())
        {
            p_Error = "download failed (status " + std::to_string(status) + ")";
        }
        return "";
    }
    std::string folder;
    PWSTR downloads = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &downloads) == S_OK && downloads)
    {
        folder = Narrow(downloads);
    }
    CoTaskMemFree(downloads);
    if (folder.empty())
    {
        wchar_t temp[MAX_PATH + 1] = {};
        GetTempPathW(MAX_PATH, temp);
        folder = Narrow(temp);
    }
    std::string name = p_Url.substr(p_Url.find_last_of('/') + 1);
    name = name.substr(0, name.find('?'));
    const std::string path = JoinPath(folder, name.empty() ? "Insta360Stitch-setup.exe" : name);
    FILE* f = OpenFile(path, "wb");
    if (!f || fwrite(body.data(), 1, body.size(), f) != body.size())
    {
        if (f)
        {
            fclose(f);
        }
        p_Error = "could not save the download in " + folder;
        return "";
    }
    fclose(f);
    return path;
#else
    (void)p_Url;
    p_Error = "downloads are only built for Windows and macOS";
    return "";
#endif
}

void OpenWithSystem(const std::string& p_PathOrUrl)
{
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", Widen(p_PathOrUrl).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    (void)p_PathOrUrl;
#endif
}
