#include "platform/windows_media_session.h"

#include <windows.h>
#include <shlobj.h>

#include <cwchar>
#include <iostream>

int main() {
    ap::platform::configure_process_app_user_model_id();

    PWSTR value = nullptr;
    const HRESULT result =
        GetCurrentProcessExplicitAppUserModelID(&value);
    const bool matches = SUCCEEDED(result) && value &&
        std::wcscmp(value, L"Jankim366.Macast.AirPlay") == 0;
    if (value) CoTaskMemFree(value);

    if (!matches) {
        std::cerr << "FAIL: process AppUserModelID was not configured before "
                     "window creation\n";
        return 1;
    }
    std::cout << "PASS: process AppUserModelID configured for SMTC\n";
    return 0;
}
