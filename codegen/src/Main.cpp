#include "Shared.hpp"

#include <filesystem>
#include <fmt/std.h>

using namespace codegen;

std::map<void const*, size_t> codegen::idMap;

std::string generateMacHeader(std::string filebase, std::string file_ext) {
    auto header_guard = file_ext == "cpp" ? "" : "#pragma once";

    return fmt::format(R"GEN({2}
#include <Geode/platform/platform.hpp>

#ifdef GEODE_IS_ARM_MAC
#include "{0}Arm.{1}"
#else
#include "{0}Intel.{1}"
#endif
)GEN", filebase, file_ext, header_guard);
}

std::string generateMacFolderHeader(std::string folder, std::string filename) {
    return fmt::format(R"GEN(#pragma once
#include <Geode/platform/platform.hpp>

#ifdef GEODE_IS_ARM_MAC
#include <Geode/{0}_arm/{1}>
#else
#include <Geode/{0}_intel/{1}>
#endif
)GEN", folder, filename);
}
/*
void generateFolderAlias(std::filesystem::path const& writeDir, std::string baseName, std::unordered_set<std::string> const& files) {
    auto outputDir = writeDir / baseName;

    for (const auto& filename : files) {
        writeFile(outputDir / filename, generateMacFolderHeader(baseName, filename));
    }
}
*/
int main(int argc, char** argv) try {
    if (argc < 4) {
        throw codegen::error("Invalid number of parameters (expected 3 or more, found {})", argc - 1);
    }

    std::string p = argv[1];

    // if (p == "--into-json") {
    //     auto rootDir = std::filesystem::path(argv[2]);
    //     std::filesystem::current_path(rootDir);

    //     Root root = broma::parse_file("Entry.bro");
    //     writeFile(std::filesystem::path(argv[3]), generateTextInterface(root));
    //     return 0;
    // }

    if (p == "Win32" || p == "Win64")  {
        codegen::platform = Platform::Windows;
        if (p == "Win32")
            codegen::platformArch = PlatformArch::x86;
    }
    else if (p == "MacOS") codegen::platform = Platform::Mac;
    else if (p == "iOS") codegen::platform = Platform::iOS;
    else if (p == "Android32") codegen::platform = Platform::Android32;
    else if (p == "Android64") codegen::platform = Platform::Android64;
    else throw codegen::error("Invalid platform {}\n", p);


    auto writeDir = std::filesystem::path(argv[3]) / "Geode";
    std::filesystem::create_directories(writeDir);

    // parse extra arguments
    std::vector<std::string> extraArgs;
    for (int i = 4; i < argc; i++) {
        std::string arg = argv[i];
        size_t space;
        while ((space = arg.find(" ")) != std::string::npos) {
            extraArgs.push_back(arg.substr(0, space));
            arg.erase(0, space + 1);
        }
        extraArgs.push_back(arg);
    }

    bool skipPugixml = false;
    bool versionSet = false;
    for (int i = 0; i < extraArgs.size(); i++) {
        auto& arg = extraArgs[i];
        if (arg == "--skip-pugixml") {
            skipPugixml = true;
        } else if (arg.starts_with("--sdk-version")) {
            if (arg.starts_with("--sdk-version=")) {
                codegen::sdkVersion = codegen::Version::fromString(arg.substr(14));
                versionSet = true;
            } else if (arg == "--sdk-version" && i + 1 < extraArgs.size()) {
                codegen::sdkVersion = codegen::Version::fromString(extraArgs[++i]);
                versionSet = true;
            }
        }
    }

    std::filesystem::create_directories(writeDir / "modify");
    std::filesystem::create_directories(writeDir / "binding");

    auto rootDir = std::filesystem::path(argv[2]);
    Root root = broma::parse_file(rootDir / "Entry.bro");

    for (auto cls : root.classes) {
        for (auto dep : cls.attributes.depends) {
            if (!is_cocos_or_fmod_class(dep) &&
                std::find(root.classes.begin(), root.classes.end(), dep) == root.classes.end()) {
                throw codegen::error("Class {} depends on unknown class {}", cls.name, dep);
            }
        }
    }

    codegen::populateIds(root);

    if (!versionSet) {
        if (auto sdkPath = std::getenv("GEODE_SDK")) {
            auto versionPath = std::filesystem::path(sdkPath) / "VERSION";
            if (std::filesystem::exists(versionPath)) {
                std::ifstream versionFile(versionPath);
                std::string version;
                std::getline(versionFile, version);
                codegen::sdkVersion = codegen::Version::fromString(version);
            }
        }
    }

    if (codegen::platform == Platform::Mac) {
        // on macos, build both platform headers together and then let the preprocessor handle the platform selection
        // this is easier. but not by a lot

        codegen::platform = Platform::MacArm;
        auto modifyArm = generateModifyHeader(root);
        codegen::platform = Platform::MacIntel;
        auto modifyIntel = generateModifyHeader(root);
        codegen::platform = Platform::Mac;

        std::string generatedModifyArm;
        std::string generatedModifyIntel;
        bool modifyDirectoriesCreated = false;
        for (auto& [filename, single_output] : modifyArm) {
            auto found = modifyIntel.find(filename);
            if (found != modifyIntel.end() && found->second == single_output) {
                writeFile(writeDir / "modify" / filename, single_output);
                modifyIntel.erase(found);
                auto modifyInclude = fmt::format("#include \"modify/{}\"\n", filename);
                generatedModifyArm += modifyInclude;
                generatedModifyIntel += modifyInclude;
            } else {
                if (!modifyDirectoriesCreated) {
                    std::filesystem::create_directories(writeDir / "modify_arm");
                    std::filesystem::create_directories(writeDir / "modify_intel");
                    modifyDirectoriesCreated = true;
                }
                writeFile(writeDir / "modify_arm" / filename, single_output);
                generatedModifyArm += fmt::format("#include \"modify_arm/{}\"\n", filename);
            }
        }

        for (auto& [filename, single_output] : modifyIntel) {
            writeFile(writeDir / "modify_intel" / filename, single_output);
            writeFile(writeDir / "modify" / filename, generateMacFolderHeader("modify", filename));
            generatedModifyIntel += fmt::format("#include \"modify_intel/{}\"\n", filename);
        }

        if (generatedModifyArm == generatedModifyIntel) {
            writeFile(writeDir / "GeneratedModify.hpp", generatedModifyArm);
        }
        else {
            writeFile(writeDir / "GeneratedModifyArm.hpp", generatedModifyArm);
            writeFile(writeDir / "GeneratedModifyIntel.hpp", generatedModifyIntel);
            writeFile(writeDir / "GeneratedModify.hpp", generateMacHeader("GeneratedModify", "hpp"));
        }

        codegen::platform = Platform::MacArm;
        auto bindingArm = generateBindingHeader(root);
        codegen::platform = Platform::MacIntel;
        auto bindingIntel = generateBindingHeader(root);
        codegen::platform = Platform::Mac;

        std::string generatedBindingArm;
        std::string generatedBindingIntel;
        bool bindingDirectoriesCreated = false;
        for (auto& [filename, single_output] : bindingArm) {
            auto found = bindingIntel.find(filename);
            if (found != bindingIntel.end() && found->second == single_output) {
                writeFile(writeDir / "binding" / filename, single_output);
                bindingIntel.erase(found);
                auto bindingInclude = fmt::format("#include \"binding/{}\"\n", filename);
                generatedBindingArm += bindingInclude;
                generatedBindingIntel += bindingInclude;
            } else {
                if (!bindingDirectoriesCreated) {
                    std::filesystem::create_directories(writeDir / "binding_arm");
                    std::filesystem::create_directories(writeDir / "binding_intel");
                    bindingDirectoriesCreated = true;
                }
                writeFile(writeDir / "binding_arm" / filename, single_output);
                generatedBindingArm += fmt::format("#include \"binding_arm/{}\"\n", filename);
            }
        }

        for (auto& [filename, single_output] : bindingIntel) {
            writeFile(writeDir / "binding_intel" / filename, single_output);
            writeFile(writeDir / "binding" / filename, generateMacFolderHeader("binding", filename));
            generatedBindingIntel += fmt::format("#include \"binding_intel/{}\"\n", filename);
        }

        if (generatedBindingArm == generatedBindingIntel) {
            writeFile(writeDir / "GeneratedBinding.hpp", generatedBindingArm);
        }
        else {
            writeFile(writeDir / "GeneratedBindingArm.hpp", generatedBindingArm);
            writeFile(writeDir / "GeneratedBindingIntel.hpp", generatedBindingIntel);
            writeFile(writeDir / "GeneratedBinding.hpp", generateMacHeader("GeneratedBinding", "hpp"));
        }

        codegen::platform = Platform::MacArm;
        auto generatedPredeclareArm = generatePredeclareHeader(root);
        codegen::platform = Platform::MacIntel;
        auto generatedPredeclareIntel = generatePredeclareHeader(root);
        codegen::platform = Platform::Mac;

        if (generatedPredeclareArm == generatedPredeclareIntel) {
            writeFile(writeDir / "GeneratedPredeclare.hpp", generatedPredeclareArm);
        }
        else {
            writeFile(writeDir / "GeneratedPredeclareArm.hpp", generatedPredeclareArm);
            writeFile(writeDir / "GeneratedPredeclareIntel.hpp", generatedPredeclareIntel);
            writeFile(writeDir / "GeneratedPredeclare.hpp", generateMacHeader("GeneratedPredeclare", "hpp"));
        }

        codegen::platform = Platform::MacArm;
        auto generatedSourceArm = generateBindingSource(root, skipPugixml);
        codegen::platform = Platform::MacIntel;
        auto generatedSourceIntel = generateBindingSource(root, skipPugixml);
        codegen::platform = Platform::Mac;

        bool generatedSourceChanged = false;
        if (generatedSourceArm == generatedSourceIntel) {
            writeFile(writeDir / "GeneratedSource.cpp", generatedSourceArm);
        }
        else {
            if (writeFile(writeDir / "GeneratedSourceArm.cpp", generatedSourceArm)) {
                generatedSourceChanged = true;
            }
            if (writeFile(writeDir / "GeneratedSourceIntel.cpp", generatedSourceIntel)) {
                generatedSourceChanged = true;
            }
            writeFile(writeDir / "GeneratedSource.cpp", generateMacHeader("GeneratedSource", "cpp"));
        }

        auto now = std::chrono::file_clock::now();
        if (generatedSourceChanged) {
            // force cmake to rebuild generatedsource
            std::filesystem::last_write_time(writeDir / "GeneratedSource.cpp", now);
        }

        codegen::platform = Platform::MacArm;
        writeFile(writeDir / "CodegenDataArm.txt", generateTextInterface(root));
        codegen::platform = Platform::MacIntel;
        writeFile(writeDir / "CodegenDataIntel.txt", generateTextInterface(root));
        codegen::platform = Platform::Mac;
    } else {
        // writeFile(writeDir / "GeneratedAddress.cpp", generateAddressHeader(root));
        auto modifyMap = generateModifyHeader(root);
        std::string generatedModify;
        for (auto& [filename, single_output] : modifyMap) {
            writeFile(writeDir / "modify" / filename, single_output);
            generatedModify += fmt::format("#include \"modify/{}\"\n", filename);
        }
        writeFile(writeDir / "GeneratedModify.hpp", generatedModify);
        auto bindingMap = generateBindingHeader(root);
        std::string generatedBinding;
        for (auto& [filename, single_output] : bindingMap) {
            writeFile(writeDir / "binding" / filename, single_output);
            generatedBinding += fmt::format("#include \"binding/{}\"\n", filename);
        }
        writeFile(writeDir / "GeneratedBinding.hpp", generatedBinding);
        writeFile(writeDir / "GeneratedPredeclare.hpp", generatePredeclareHeader(root));
        writeFile(writeDir / "GeneratedSource.cpp", generateBindingSource(root, skipPugixml));
        writeFile(writeDir / "CodegenData.txt", generateTextInterface(root));
    }

    writeFile(writeDir / "CodegenData.json", generateJsonInterface(root).dump(0));
}

catch (std::exception& e) {
    std::cout << "Codegen error: " << e.what() << "\n";
    return 1;
}
