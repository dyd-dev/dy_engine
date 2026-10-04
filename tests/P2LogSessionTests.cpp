#include "../src/Platform/LogSession.h"
#include <cstdio>

// A fresh process gives the real once-initialized session a fresh environment.
// The Python runner checks its directory, persisted log and shutdown metadata.
int main()
{
    dyf::Platform::LogInternal::InitializeSession();
    const auto directory = dyf::Platform::LogInternal::SessionDirectory();
    const bool enabled = dyf::Platform::LogInternal::HasSessionOutput();
    dyf::Platform::LogInternal::WriteSession("P2 log session path check\n");
    std::printf("%d\n%s\n", enabled ? 1 : 0, directory.c_str());
}
