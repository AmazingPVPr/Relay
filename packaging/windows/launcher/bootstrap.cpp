#include <windows.h>
#include <string>
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR arguments, int) {
    wchar_t buffer[32768];
    DWORD n=GetModuleFileNameW(nullptr,buffer,32768);
    if (!n || n>=32768) return 1;
    std::wstring root(buffer,n); root.resize(root.find_last_of(L"\\/"));
    std::wstring program=root+L"\\bin\\relay-launcher.exe";
    std::wstring command=L"\""+program+L"\" "+arguments;
    STARTUPINFOW startup{}; startup.cb=sizeof(startup); PROCESS_INFORMATION process{};
    if (!CreateProcessW(program.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,root.c_str(),&startup,&process)) {
        MessageBoxW(nullptr,L"Relay could not start. Extract the entire ZIP, keeping Relay.exe and the bin folder together.",L"Relay",MB_OK|MB_ICONERROR); return 1;
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess); return 0;
}
