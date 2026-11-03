#pragma once

#include <string>

// Shows a progress window outside Resolve (the TrackProgress helper in the bundle) while the
// plugin is busy, since Resolve's own interface does not update until the plugin returns.
// Everything fails quietly: without the helper the work simply runs without a window.
class ProgressWindow
{
public:
    ~ProgressWindow();

    void open(const std::string& p_Title);

    // Returns false once the user has pressed Cancel.
    bool update(double p_Fraction, const std::string& p_Text);

    void close();

private:
#ifdef _WIN32
    void* m_Process = nullptr;    // HANDLE of the helper
    void* m_ToHelper = nullptr;   // HANDLE, our end of its stdin
    void* m_FromHelper = nullptr; // HANDLE, our end of its stdout
#else
    int m_Pid = -1;
    int m_ToHelper = -1;
    int m_FromHelper = -1;
#endif
    bool m_Cancelled = false;
    double m_LastSent = -1.0;
};

// Where the bundle's helper programs are: <bundle>/Contents/Resources, next to the folder of
// the plugin binary. p_Name without ".exe"; it is added on Windows.
std::string BundleTool(const std::string& p_Name);
