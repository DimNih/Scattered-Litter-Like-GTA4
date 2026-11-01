#pragma once

namespace LocalDiagnostics
{
    void Open();
    void Close();
    void Write(const char* format, ...);
}
