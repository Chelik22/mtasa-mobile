#include <ExperimentalLabPaths.h>
#include <iostream>

namespace lab = experimental_lab;
void require(bool value, const char* reason)
{
    if (!value)
        throw std::runtime_error(reason);
}

int wmain(int argc, wchar_t** argv)
{
    const std::wstring mode = argc > 1 ? argv[1] : L"";
    try
    {
        const auto& context = lab::GetContext();
        if (mode == L"disabled")
        {
            require(!context.enabled, "absent opt-in must retain stock mode");
            std::cout << "PASS disabled; no settings operation\n";
            return 0;
        }
        if (mode == L"reject-root")
        {
            std::cerr << "invalid root was accepted\n";
            return 2;
        }
        require(context.enabled, "expected explicit lab context");
        const auto key = L"Software\\MTA Lab Contract\\section";
        if (mode == L"freeze")
        {
            const auto root = context.root;
            SetEnvironmentVariableW(lab::environment, L"relative-invalid-root");
            require(lab::GetContext().root == root, "captured root changed");
            bool rejected = false;
            try
            {
                context.Child(L"../outside");
            }
            catch (const std::exception&)
            {
                rejected = true;
            }
            require(rejected, "nonfixed child escaped the namespace");
        }
        else if (mode == L"write")
        {
            lab::WriteSetting(HKEY_LOCAL_MACHINE, key, L"Persist", "actual-persisted-value");
            lab::WriteSetting(HKEY_LOCAL_MACHINE, key, L"Case", "case-value");
            require(lab::ReadSetting(HKEY_LOCAL_MACHINE, L"software\\mta lab contract\\SECTION", L"cASE") == "case-value",
                    "registry key casing changed identity");
            lab::WriteSetting(HKEY_LOCAL_MACHINE, L"Software\\MTA Lab Contract\\section\\child", L"value", "child-value");
            require(!lab::DeleteSettingsKey(HKEY_LOCAL_MACHINE, key), "parent containing a subkey was deleted");
            require(lab::DeleteSettingsKey(HKEY_LOCAL_MACHINE, L"Software\\MTA Lab Contract\\section\\child"), "child delete failed");
            require(lab::ReadSetting(HKEY_LOCAL_MACHINE, key, L"Persist") == "actual-persisted-value", "unrelated entry was lost");
        }
        else if (mode == L"read")
        {
            require(lab::ReadSetting(HKEY_LOCAL_MACHINE, key, L"persist") == "actual-persisted-value", "independent process cannot read prior settings");
        }
        else if (mode == L"oversize")
        {
            bool rejected = false;
            try
            {
                lab::WriteSetting(HKEY_LOCAL_MACHINE, key, L"too-large", std::string(lab::max_value + 1, 'x'));
            }
            catch (const std::exception&)
            {
                rejected = true;
            }
            require(rejected, "oversize value accepted");
        }
        else if (mode == L"corrupt")
        {
            bool rejected = false;
            try
            {
                lab::ReadSetting(HKEY_LOCAL_MACHINE, key, L"persist");
            }
            catch (const std::exception&)
            {
                rejected = true;
            }
            require(rejected, "corrupt settings were silently accepted");
        }
        else if (mode == L"writer")
        {
            require(argc == 3, "writer needs identity");
            for (int index = 0; index < 20; ++index)
                lab::WriteSetting(HKEY_LOCAL_MACHINE, key, argv[2], std::to_string(index));
        }
        else if (mode == L"concurrent-result")
        {
            require(lab::ReadSetting(HKEY_LOCAL_MACHINE, key, L"writer-a") == "19", "writer A entry was lost");
            require(lab::ReadSetting(HKEY_LOCAL_MACHINE, key, L"writer-b") == "19", "writer B entry was lost");
            require(lab::ReadSetting(HKEY_LOCAL_MACHINE, key, L"Persist") == "actual-persisted-value", "concurrent update lost old data");
        }
        else
            throw std::runtime_error("unknown probe mode");
        std::cout << "PASS lab filesystem contract; GTA/MTA/registry API execution NOT RUN\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        if (mode == L"reject-root")
        {
            std::cout << "PASS explicit invalid root rejected: " << error.what() << '\n';
            return 0;
        }
        std::cerr << error.what() << '\n';
        return 1;
    }
}
