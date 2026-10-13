// A shared object in a plugin folder that is not a plugin (no setInfo ...):
// Notepad++ reports it and goes on with the others.
extern "C" __attribute__((visibility("default"))) const wchar_t* getName()
{
    return L"Not a plugin";
}
