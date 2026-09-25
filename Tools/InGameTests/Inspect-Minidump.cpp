#include <Windows.h>
#include <DbgHelp.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{
std::wstring ReadDumpString(const std::byte* base, const RVA rva)
{
    if (!rva)
        return {};
    const auto* value = reinterpret_cast<const MINIDUMP_STRING*>(base + rva);
    return {value->Buffer, value->Length / sizeof(wchar_t)};
}

const std::byte* FindMemory(
    const std::byte* base, const MINIDUMP_MEMORY64_LIST* memories, const uint64_t address, size_t& available)
{
    uint64_t fileRva = memories->BaseRva;
    for (ULONG64 i = 0; i < memories->NumberOfMemoryRanges; ++i)
    {
        const auto& range = memories->MemoryRanges[i];
        if (address >= range.StartOfMemoryRange && address < range.StartOfMemoryRange + range.DataSize)
        {
            available = static_cast<size_t>(range.StartOfMemoryRange + range.DataSize - address);
            return base + fileRva + (address - range.StartOfMemoryRange);
        }
        fileRva += range.DataSize;
    }
    return nullptr;
}

const std::byte* FindMemory(
    const std::byte* base, const MINIDUMP_MEMORY_LIST* memories, const uint64_t address, size_t& available)
{
    for (ULONG32 i = 0; i < memories->NumberOfMemoryRanges; ++i)
    {
        const auto& range = memories->MemoryRanges[i];
        if (address >= range.StartOfMemoryRange && address < range.StartOfMemoryRange + range.Memory.DataSize)
        {
            available = static_cast<size_t>(range.StartOfMemoryRange + range.Memory.DataSize - address);
            return base + range.Memory.Rva + (address - range.StartOfMemoryRange);
        }
    }
    return nullptr;
}

void PrintSymbol(const HANDLE process, const uint64_t address)
{
    std::vector<std::byte> storage(sizeof(SYMBOL_INFO) + MAX_SYM_NAME);
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage.data());
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement = 0;
    if (!SymFromAddr(process, address, &displacement, symbol))
        return;

    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisplacement = 0;
    std::cout << "  0x" << std::hex << address << " " << symbol->Name << "+0x" << displacement;
    if (SymGetLineFromAddr64(process, address, &lineDisplacement, &line))
        std::cout << " (" << line.FileName << ':' << std::dec << line.LineNumber << ')';
    std::cout << '\n';
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 4 && argc != 6)
    {
        std::wcerr << L"Usage: Inspect-Minidump.exe <dump> <matching-image> <symbol-directory> [address length<=256]\n";
        return 2;
    }

    const HANDLE file = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return 3;
    const HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    const auto* base = reinterpret_cast<const std::byte*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    if (!base)
        return 4;

    PMINIDUMP_DIRECTORY directory = nullptr;
    void* stream = nullptr;
    ULONG streamSize = 0;
    if (!MiniDumpReadDumpStream(const_cast<std::byte*>(base), ExceptionStream, &directory, &stream, &streamSize))
        return 5;
    const auto* exception = static_cast<const MINIDUMP_EXCEPTION_STREAM*>(stream);

    if (!MiniDumpReadDumpStream(const_cast<std::byte*>(base), ModuleListStream, &directory, &stream, &streamSize))
        return 6;
    const auto* modules = static_cast<const MINIDUMP_MODULE_LIST*>(stream);

    const MINIDUMP_MODULE* faultModule = nullptr;
    std::wstring faultModuleName;
    for (ULONG32 i = 0; i < modules->NumberOfModules; ++i)
    {
        const auto& module = modules->Modules[i];
        if (exception->ExceptionRecord.ExceptionAddress >= module.BaseOfImage &&
            exception->ExceptionRecord.ExceptionAddress < module.BaseOfImage + module.SizeOfImage)
        {
            faultModule = &module;
            faultModuleName = ReadDumpString(base, module.ModuleNameRva);
            break;
        }
    }

    std::wcout << L"Exception 0x" << std::hex << exception->ExceptionRecord.ExceptionCode << L" at 0x"
               << exception->ExceptionRecord.ExceptionAddress << L" on thread " << std::dec << exception->ThreadId << L'\n';
    if (faultModule)
        std::wcout << L"Fault module: " << faultModuleName << L" + 0x" << std::hex
                   << (exception->ExceptionRecord.ExceptionAddress - faultModule->BaseOfImage) << L'\n';
    if (exception->ExceptionRecord.NumberParameters >= 2)
        std::wcout << L"Access: " << (exception->ExceptionRecord.ExceptionInformation[0] ? L"write/execute" : L"read")
                   << L" at 0x" << std::hex << exception->ExceptionRecord.ExceptionInformation[1] << L'\n';

    CONTEXT context{};
    if (exception->ThreadContext.DataSize >= sizeof(context))
        std::memcpy(&context, base + exception->ThreadContext.Rva, sizeof(context));
    std::wcout << L"RIP=0x" << std::hex << context.Rip << L" RSP=0x" << context.Rsp << L" RBP=0x" << context.Rbp << L'\n';
    std::wcout << L"RAX=0x" << context.Rax << L" RBX=0x" << context.Rbx
               << L" RCX=0x" << context.Rcx << L" RDX=0x" << context.Rdx
               << L" RSI=0x" << context.Rsi << L" RDI=0x" << context.Rdi
               << L" R8=0x" << context.R8 << L" R9=0x" << context.R9
               << L" R10=0x" << context.R10 << L" R11=0x" << context.R11
               << L" R12=0x" << context.R12 << L" R13=0x" << context.R13
               << L" R14=0x" << context.R14 << L" R15=0x" << context.R15 << L'\n';

    const std::byte* instructions = nullptr;
    size_t instructionBytes = 0;
    if (MiniDumpReadDumpStream(const_cast<std::byte*>(base), Memory64ListStream, &directory, &stream, &streamSize))
    {
        instructions = FindMemory(base, static_cast<const MINIDUMP_MEMORY64_LIST*>(stream),
            context.Rip, instructionBytes);
    }
    if (!instructions && MiniDumpReadDumpStream(const_cast<std::byte*>(base), MemoryListStream,
            &directory, &stream, &streamSize))
        instructions = FindMemory(base, static_cast<const MINIDUMP_MEMORY_LIST*>(stream),
            context.Rip, instructionBytes);
    if (instructions)
    {
        std::cout << "Instruction bytes:";
        for (size_t i = 0; i < (std::min)(instructionBytes, size_t{24}); ++i)
            std::cout << ' ' << std::hex << static_cast<unsigned>(instructions[i]);
        std::cout << '\n';
    }

    if (argc == 6)
    {
        wchar_t* addressEnd = nullptr;
        wchar_t* lengthEnd = nullptr;
        const auto address = std::wcstoull(argv[4], &addressEnd, 0);
        const auto length = std::wcstoull(argv[5], &lengthEnd, 0);
        if (*addressEnd || *lengthEnd || !address || !length || length > 256)
            return 8;
        const std::byte* memory = nullptr;
        size_t available = 0;
        if (MiniDumpReadDumpStream(const_cast<std::byte*>(base), Memory64ListStream,
                &directory, &stream, &streamSize))
            memory = FindMemory(base, static_cast<const MINIDUMP_MEMORY64_LIST*>(stream),
                address, available);
        if (!memory && MiniDumpReadDumpStream(const_cast<std::byte*>(base), MemoryListStream,
                &directory, &stream, &streamSize))
            memory = FindMemory(base, static_cast<const MINIDUMP_MEMORY_LIST*>(stream),
                address, available);
        std::cout << "Memory at 0x" << std::hex << address << ':';
        if (!memory)
            std::cout << " unavailable in dump\n";
        else
        {
            const auto count = (std::min)(static_cast<size_t>(length), available);
            for (size_t i = 0; i < count; ++i)
            {
                if (i % 16 == 0)
                    std::cout << "\n  +0x" << std::hex << i << ':';
                std::cout << ' ' << std::hex << static_cast<unsigned>(memory[i]);
            }
            std::cout << '\n';
        }
    }

    const HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (!SymInitializeW(process, argv[3], FALSE))
        return 7;

    const auto imageName = std::filesystem::path(argv[2]).filename().wstring();
    const MINIDUMP_MODULE* imageModule = nullptr;
    for (ULONG32 i = 0; i < modules->NumberOfModules; ++i)
    {
        const auto dumpName = std::filesystem::path(ReadDumpString(base, modules->Modules[i].ModuleNameRva)).filename().wstring();
        if (_wcsicmp(dumpName.c_str(), imageName.c_str()) == 0)
        {
            imageModule = &modules->Modules[i];
            break;
        }
    }
    if (!imageModule || !SymLoadModuleExW(process, nullptr, argv[2], nullptr, imageModule->BaseOfImage, imageModule->SizeOfImage, nullptr, 0))
        std::wcerr << L"Could not load matching image symbols, error " << GetLastError() << L'\n';

    std::cout << "Resolved exception:\n";
    PrintSymbol(process, exception->ExceptionRecord.ExceptionAddress);

    const std::byte* stack = nullptr;
    size_t available = 0;
    if (MiniDumpReadDumpStream(const_cast<std::byte*>(base), Memory64ListStream, &directory, &stream, &streamSize))
    {
        const auto* memories = static_cast<const MINIDUMP_MEMORY64_LIST*>(stream);
        stack = FindMemory(base, memories, context.Rsp, available);
    }
    if (!stack && MiniDumpReadDumpStream(const_cast<std::byte*>(base), MemoryListStream, &directory, &stream, &streamSize))
    {
        const auto* memories = static_cast<const MINIDUMP_MEMORY_LIST*>(stream);
        stack = FindMemory(base, memories, context.Rsp, available);
    }
    if (stack)
    {
        constexpr size_t maximumStackWords = 4096;
        std::cout << "Candidate frames from stack memory:\n";
        const auto* words = reinterpret_cast<const uint64_t*>(stack);
        const auto stackWords = (std::min)(maximumStackWords, available / sizeof(uint64_t));
        size_t candidates = 0;
        for (size_t i = 0; i < stackWords && candidates < 80; ++i)
        {
            for (ULONG32 j = 0; j < modules->NumberOfModules; ++j)
            {
                const auto& module = modules->Modules[j];
                if (words[i] < module.BaseOfImage || words[i] >= module.BaseOfImage + module.SizeOfImage)
                    continue;
                const auto name = std::filesystem::path(ReadDumpString(base, module.ModuleNameRva)).filename().wstring();
                std::wcout << L"  stack+0x" << std::hex << i * sizeof(uint64_t) << L"  " << name
                           << L"+0x" << words[i] - module.BaseOfImage << L'\n';
                PrintSymbol(process, words[i]);
                ++candidates;
                break;
            }
        }
    }

    SymCleanup(process);
    UnmapViewOfFile(base);
    CloseHandle(mapping);
    CloseHandle(file);
    return 0;
}
