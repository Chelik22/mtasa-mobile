#include <dxerr.h>

// Link all three real Microsoft functions; this program is never launched by preparation.
int main()
{
    wchar_t description[1024]{};
    DXGetErrorDescriptionW(E_FAIL, description, 1024);
    const wchar_t* name = DXGetErrorStringW(E_FAIL);
    const HRESULT  result = DXTraceW(L"sdk-link-probe", 0, E_FAIL, L"No message box", false);
    return name && description[0] && result == E_FAIL ? 0 : 1;
}
