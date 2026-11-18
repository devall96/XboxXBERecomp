#pragma once
// What differs between Windows and Linux under the app layer: the program's path and command line, threads, message
// boxes, text conversion. Files that are Windows only (the D3D12 window, XAudio2) keep their own calls.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace platform {

std::filesystem::path ExePath();
// the command line as one string, the program's name first; Linux joins main's arguments (SetArgs) with spaces
std::wstring CommandLine();
void SetArgs(int argc, char** argv);  // Linux: main's arguments, before anything reads CommandLine
const std::vector<std::string>& Args();  // Linux: main's arguments; empty on Windows
// the number after an argument ("--instance 2" or "--instance2"), def without the argument
int ArgInt(const wchar_t* name, int def);
// "--name <value>" or "--name=<value>"; empty when it is not given
std::wstring ArgString(const wchar_t* name);
bool HasArg(const wchar_t* name);

// a detached thread with stack bytes for its stack: recompiled code nests host frames as deep as the guest's calls
void SpawnThread(size_t stack, std::function<void()> fn);
void NameThread(const char* name);  // shown by debuggers and profilers
void SleepMs(uint32_t ms);
uint32_t ProcessId();

// a message box on its own (no parent window); AskYesNo defaults to No
void Message(const wchar_t* title, const std::wstring& text, bool error);  // also in the log (and stderr on Linux)
// Linux: what to install when the PC offers no Vulkan 1.3 driver, for the player
std::wstring VulkanDriverHelp();
bool AskYesNo(const wchar_t* title, const std::wstring& text);

std::string Utf8(const std::wstring& w);
std::wstring Wide(const std::string& utf8);
std::wstring Env(const wchar_t* name);  // empty when unset

}  // namespace platform
