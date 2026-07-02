#include <algorithm>
#include <format>
#include <fstream>
#include <string>
#include "Events.h"
#include "SKSEMCP/SKSEMenuFramework.hpp"
#include "rapidjson/document.h"
#include "rapidjson/filereadstream.h"
#include "rapidjson/filewritestream.h"
#include "rapidjson/prettywriter.h"
#include "MCP.h"
#include "OARAPI.h"
#include "Serialization.h"
#include "ClibUtil/editorID.hpp"
#include "Hooks.h"

    // Helper function to copy a single file with logs
    void CopySingleFile(const std::filesystem::path& sourceFile, const std::filesystem::path& destinationPath,
                        int& filesCopied) {
        try {
            std::filesystem::copy_file(sourceFile, destinationPath / sourceFile.filename(),
                                       std::filesystem::copy_options::overwrite_existing);
            filesCopied++;
        } catch (const std::filesystem::filesystem_error& e) {
            SKSE::log::error("Failed to copy file: {}. Error: {}", sourceFile.string(), e.what());
        }
    }

    void ProcessCycleDarFile(const std::filesystem::path& cycleDarJsonPath) {
        SKSE::log::info("Processing CycleDar.json at: {}", cycleDarJsonPath.string());

        // 1. Open and read the JSON file
        std::ifstream fileStream(cycleDarJsonPath);
        if (!fileStream) {
            SKSE::log::error("Failed to open {}", cycleDarJsonPath.string());
            return;
        }
        std::string jsonContent((std::istreambuf_iterator<char>(fileStream)), std::istreambuf_iterator<char>());
        fileStream.close();

        // 2. Parse the JSON
        rapidjson::Document doc;
        doc.Parse(jsonContent.c_str());

        if (doc.HasParseError()) {
            SKSE::log::error("JSON parse error in {}", cycleDarJsonPath.string());
            return;
        }

        // 3. Check if the conversion has already been done
        if (doc.HasMember("conversionDone") && doc["conversionDone"].IsBool() && doc["conversionDone"].GetBool()) {
            SKSE::log::info("The copy to {} was already completed previously. Skipping.", cycleDarJsonPath.string());
            return;
        }

        int filesCopied = 0;
        std::filesystem::path destinationPath = cycleDarJsonPath.parent_path();

        // NEW LOGIC FOR PROCESSING MULTIPLE SOURCES
        auto processSource = [&](const std::string& relativePath, const rapidjson::Value* filesToCopyArray) {
            std::filesystem::path sourcePath = "Data" / std::filesystem::path(relativePath);
            if (!std::filesystem::exists(sourcePath) || !std::filesystem::is_directory(sourcePath)) {
                SKSE::log::warn("Source folder does not exist or is not a directory: {}", sourcePath.string());
                return;
            }

            SKSE::log::info("Copying files from '{}' to '{}'", sourcePath.string(), destinationPath.string());

            if (filesToCopyArray && filesToCopyArray->IsArray() && !filesToCopyArray->Empty()) {
                // Mode: Copy files specified in the list
                SKSE::log::info("Mode: Copying files specified in the 'filesToCopy' list.");
                for (const auto& fileValue : filesToCopyArray->GetArray()) {
                    if (fileValue.IsString()) {
                        std::filesystem::path sourceFile = sourcePath / fileValue.GetString();
                        if (std::filesystem::exists(sourceFile)) {
                            CopySingleFile(sourceFile, destinationPath, filesCopied);
                        } else {
                            SKSE::log::warn("Specified file not found in source: {}", sourceFile.string());
                        }
                    }
                }
            } else {
                // Mode: Copy all .hkx files (default behavior)
                SKSE::log::info("Mode: Copying all .hkx files from the folder.");
                for (const auto& fileEntry : std::filesystem::directory_iterator(sourcePath)) {
                    if (fileEntry.is_regular_file()) {
                        std::string extension = fileEntry.path().extension().string();
                        std::transform(extension.begin(), extension.end(), extension.begin(),
                                       [](unsigned char c) { return std::tolower(c); });
                        if (extension == ".hkx") {
                            CopySingleFile(fileEntry.path(), destinationPath, filesCopied);
                        }
                    }
                }
            }
        };

        if (doc.HasMember("sources") && doc["sources"].IsArray()) {
            // NEW FORMAT: Process the "sources" array
            for (const auto& sourceObj : doc["sources"].GetArray()) {
                if (sourceObj.IsObject() && sourceObj.HasMember("path") && sourceObj["path"].IsString()) {
                    const rapidjson::Value* filesArray =
                        sourceObj.HasMember("filesToCopy") ? &sourceObj["filesToCopy"] : nullptr;
                    processSource(sourceObj["path"].GetString(), filesArray);
                }
            }
        } else if (doc.HasMember("pathDar") && doc["pathDar"].IsString()) {
            // OLD FORMAT (LEGACY): To maintain compatibility
            const rapidjson::Value* filesArray = doc.HasMember("filesToCopy") ? &doc["filesToCopy"] : nullptr;
            processSource(doc["pathDar"].GetString(), filesArray);
        } else {
            SKSE::log::error("Invalid or unrecognized CycleDar.json format in {}", cycleDarJsonPath.string());
            return;
        }

        SKSE::log::info("Copy complete. {} files moved.", filesCopied);

        // BFCO conversion logic (unchanged)
        bool shouldConvertToBFCO = false;
        if (doc.HasMember("convertBFCO") && doc["convertBFCO"].IsBool()) {
            shouldConvertToBFCO = doc["convertBFCO"].GetBool();
        }

        if (shouldConvertToBFCO && filesCopied > 0) {
            SKSE::log::info("Starting conversion from MCO to BFCO...");
            int filesRenamed = 0;
            for (const auto& fileEntry : std::filesystem::directory_iterator(destinationPath)) {
                if (fileEntry.is_regular_file()) {
                    std::string filename = fileEntry.path().filename().string();
                    std::string lower_filename = filename;
                    // 2. Convert the copy to lowercase.
                    std::transform(lower_filename.begin(), lower_filename.end(), lower_filename.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    if (lower_filename.rfind("mco_", 0) == 0) {
                        std::string newFilename = filename;
                        newFilename.replace(0, 4, "BFCO_");
                        std::filesystem::path newFilePath = destinationPath / newFilename;
                        try {
                            std::filesystem::rename(fileEntry.path(), newFilePath);
                            filesRenamed++;
                        } catch (const std::filesystem::filesystem_error& e) {
                            SKSE::log::error("Failed to rename {} to {}. Error: {}", fileEntry.path().string(),
                                             newFilePath.string(), e.what());
                        }
                    }
                }
            }
            SKSE::log::info("BFCO conversion complete. {} files renamed.", filesRenamed);
        }

        // JSON update logic (unchanged)
        if (doc.HasMember("conversionDone")) {
            doc["conversionDone"].SetBool(true);
        } else {
            doc.AddMember("conversionDone", true, doc.GetAllocator());
        }

        rapidjson::StringBuffer buffer;
        rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
        doc.Accept(writer);

        std::ofstream outFile(cycleDarJsonPath);
        if (!outFile) {
            SKSE::log::error("Failed to open {} for writing!", cycleDarJsonPath.string());
            return;
        }

        outFile << buffer.GetString();
        outFile.close();

        SKSE::log::info("JSON file {} updated successfully.", cycleDarJsonPath.string());
    }

    // --- CHALLENGE 1: New function to scan the sub-moveset folder for tags ---
    void ScanSubAnimationFolderForTags(const std::filesystem::path& subAnimPath,
                                                         SubAnimationDef& subAnimDef) {
        if (!std::filesystem::exists(subAnimPath) || !std::filesystem::is_directory(subAnimPath)) {
            return;
        }

        // --- POINT 2: Process CycleDar.json BEFORE scanning tags ---
        // Look for the CycleDar.json file and copy the .hkx files if it exists.
        // This ensures the copied files will be present for the tag scanning below.
        std::filesystem::path cycleDarPath = subAnimPath / "CycleDar.json";
        if (std::filesystem::exists(cycleDarPath) && std::filesystem::is_regular_file(cycleDarPath)) {
            ProcessCycleDarFile(cycleDarPath);  // Call the new helper function
        }


        subAnimDef.attackCount = 0;
        subAnimDef.powerAttackCount = 0;
        subAnimDef.hasIdle = false;
        subAnimDef.hasAnimations = false;
        subAnimDef.dpaTags = {};
        subAnimDef.hasCPA = false;  // Initial value
        int hkxFileCount = 0;


       // Iterate over all files in the folder to find animation tags
        for (const auto& fileEntry : std::filesystem::directory_iterator(subAnimPath)) {
            if (fileEntry.is_regular_file()) {
                std::string extension = fileEntry.path().extension().string();
                std::string filename = fileEntry.path().filename().string();
                std::string lowerFilename = filename;
                std::transform(lowerFilename.begin(), lowerFilename.end(), lowerFilename.begin(), ::tolower);
                std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

                if (extension == ".hkx") {
                    hkxFileCount++;

                    // Attack counting logic
                    if (lowerFilename.rfind("bfco_attack", 0) == 0) {
                        subAnimDef.attackCount++;
                    }
                    if (lowerFilename.rfind("bfco_powerattack", 0) == 0) {
                        subAnimDef.powerAttackCount++;
                    }
                    if (lowerFilename.find("idle") != std::string::npos) {
                        subAnimDef.hasIdle = true;
                    }

                    // DPA and CPA verification logic
                    if (lowerFilename == "bfco_powerattacka.hkx")
                        subAnimDef.dpaTags.hasA = true;
                    else if (lowerFilename == "bfco_powerattackb.hkx")
                        subAnimDef.dpaTags.hasB = true;
                    else if (lowerFilename == "bfco_powerattackl.hkx")
                        subAnimDef.dpaTags.hasL = true;
                    else if (lowerFilename == "bfco_powerattackr.hkx")
                        subAnimDef.dpaTags.hasR = true;
                    else if (lowerFilename == "bfco_powerattackcomb.hkx")
                        subAnimDef.hasCPA = true;
                }
            }
        }

        if (hkxFileCount > 0) {
            subAnimDef.hasAnimations = true;
        }


         logger::info("Scan of folder '{}': hasDPA (A:{}, B:{}, L:{}, R:{}), hasCPA:{}", subAnimDef.name,
                        subAnimDef.dpaTags.hasA, subAnimDef.dpaTags.hasB, subAnimDef.dpaTags.hasL,
                        subAnimDef.dpaTags.hasR, subAnimDef.hasCPA);
    }

    // --- Scanning Logic (Loads the Library) ---
    void AnimationManager::ScanAnimationMods() {
        SKSE::log::info("Starting animation library scan...");
        _categories.clear();
        _allMods.clear();

        const std::filesystem::path oarRootPath = "Data\\meshes\\actors\\character\\animations\\OpenAnimationReplacer";
        // IMPROVED STRUCTURE: Makes it easier to define categories and their properties
        struct CategoryDefinition {
            std::string name;
            double typeValue;
            double leftHandTypeValue = -1.0;
            bool isDual;
            bool isShield;
            std::vector<std::string> keywords;
            std::vector<std::string> leftHandKeywords; 
        };

        std::vector<CategoryDefinition> categoryDefinitions = {
            // Single-Wield
            {"Sword", 1.0, 0.0, false, {}, {}},
            {"Dagger", 2.0, 0.0, false, false, {}, {}},
            {"War Axe", 3.0, 0.0, false, false, {}, {}},
            {"Mace", 4.0, 0.0, false, false, {}, {}},
            {"Greatsword", 5.0, -1.0, false, false, {}, {}},
            {"Battleaxe", 6.0, -1.0, false, false, {}, {}},
            {"Warhammer", 10.0, -1.0, false, false, {"WeapTypeWarhammer"}, {}},
            // Shield
            //{"Shield", -1.0, 11.0, false, true, {}, {}},
            {"Sword & Shield", 1.0, 11.0, false, true, {}, {}},
            {"Dagger & Shield", 2.0, 11.0, false, true, {}, {}},
            {"War Axe & Shield", 3.0, 11.0, false, true, {}, {}},
            {"Mace & Shield", 4.0, 11.0, false, true, {}, {}},
            {"Greatsword & Shield", 5.0, 11.0, false, true, {}, {}},
            {"Battleaxe & Shield", 6.0, 11.0, false, true, {}, {}},
            {"Warhammer & Shield", 10.0, 11.0, false, true, {"WeapTypeWarhammer"}, {}},
            // Dual-Wield
            {"Dual Sword", 1.0, 1.0, true, {}, {}},
            {"Dual Dagger", 2.0, 2.0, true, {}, {}},
            {"Dual War Axe", 3.0, 3.0, true, {}, {}},
            {"Dual Mace", 4.0, 4.0, true, {}, {}},
            {"Unarmed", 0.0, 0.0, true, {}, {}}
        };

        for (const auto& def : categoryDefinitions) {
            _categories[def.name].name = def.name;
            _categories[def.name].equippedTypeValue = def.typeValue;
            _categories[def.name].leftHandEquippedTypeValue = def.leftHandTypeValue;
            _categories[def.name].isDualWield = def.isDual;
            _categories[def.name].isShieldCategory = def.isShield;
            _categories[def.name].keywords = def.keywords;
            _categories[def.name].leftHandKeywords = def.leftHandKeywords;
            _categories[def.name].isCustom = false;
            _categories[def.name].baseCategoryName = "Base";

            // --- NEW: Initialize stance names and buffers ---
            for (int i = 0; i < 4; ++i) {
                std::string defaultName = std::format("Stance {}", i + 1);
                _categories[def.name].stanceNames[i] = defaultName;
                strcpy_s(_categories[def.name].stanceNameBuffers[i].data(),
                         _categories[def.name].stanceNameBuffers[i].size(), defaultName.c_str());
            }
        }
        LoadCustomCategories();
        LoadStanceNames();

        ScanDarAnimations();
        if (!_darSubMovesets.empty()) {
            AnimationModDef darModDef;
            darModDef.name = "[DAR] Animations";
            darModDef.author = "Dynamic Animation Replacer";
            darModDef.subAnimations = _darSubMovesets;  // Copy the scanned DAR animations
            _allMods.push_back(darModDef);
            SKSE::log::info("Integrated {} DAR animations as a virtual mod.", _darSubMovesets.size());
        }


        if (!std::filesystem::exists(oarRootPath)) return;
        for (const auto& entry : std::filesystem::directory_iterator(oarRootPath)) {
            if (entry.is_directory()) {
                ProcessTopLevelMod(entry.path());
            }
        }
        SKSE::log::info("File scan finished. {} mods loaded.", _allMods.size());

        // Now that we have all mods, let's find which files we already manage.
        SKSE::log::info("Checking previously managed files...");
        _managedFiles.clear();
        for (const auto& mod : _allMods) {
            for (const auto& subAnim : mod.subAnimations) {
                if (std::filesystem::exists(subAnim.path)) {
                    std::ifstream fileStream(subAnim.path);
                    std::string content((std::istreambuf_iterator<char>(fileStream)), std::istreambuf_iterator<char>());
                    fileStream.close();
                    if (content.find("OAR_CYCLE_MANAGER_CONDITIONS") != std::string::npos) {
                        _managedFiles.insert(subAnim.path);
                    }
                }
            }
        }
        SKSE::log::info("Found {} managed files.", _managedFiles.size());

        // --- NEW SECTION: Load and integrate user movesets ---
        //LoadUserMovesets();

        for (const auto& userMoveset : _userMovesets) {
            AnimationModDef modDef;
            modDef.name = userMoveset.name;
            modDef.author = "User";  // Default author

            for (const auto& subInstance : userMoveset.subAnimations) {
                // Check if the indices are valid to avoid crashes
                if (subInstance.sourceModIndex < _allMods.size()) {
                    const auto& sourceMod = _allMods[subInstance.sourceModIndex];
                    if (subInstance.sourceSubAnimIndex < sourceMod.subAnimations.size()) {
                        // Add the original sub-animation definition to our new virtual mod
                        modDef.subAnimations.push_back(sourceMod.subAnimations[subInstance.sourceSubAnimIndex]);
                    }
                }
            }
            _allMods.push_back(modDef);
        }
        SKSE::log::info("Integration finished. Total of {} mods in the library (including user mods).", _allMods.size());
        // --- NEW CALL ---
        // Now that the mod library (_allMods) is complete, we load the UI configuration.
        _npcCategories = _categories;
        LoadCycleMovesets();

        SKSE::log::info("Weapon categories for NPCs initialized.");
    }

    void AnimationManager::ProcessTopLevelMod(const std::filesystem::path& modPath) {
        std::filesystem::path configPath = modPath / "config.json";
        if (!std::filesystem::exists(configPath)) return;
        std::ifstream fileStream(configPath);
        std::string jsonContent((std::istreambuf_iterator<char>(fileStream)), std::istreambuf_iterator<char>());
        fileStream.close();
        rapidjson::Document doc;
        doc.Parse(jsonContent.c_str());
        if (doc.IsObject() && doc.HasMember("name") && doc.HasMember("author")) {
            AnimationModDef modDef;
            modDef.name = doc["name"].GetString();
            modDef.author = doc["author"].GetString();
            for (const auto& subEntry : std::filesystem::recursive_directory_iterator(modPath)) {
                if (subEntry.is_directory() && std::filesystem::exists(subEntry.path() / "config.json")) {
                    if (std::filesystem::equivalent(modPath, subEntry.path())) continue;
                    SubAnimationDef subAnimDef;
                    subAnimDef.name = subEntry.path().filename().string();
                    subAnimDef.path = subEntry.path() / "config.json";
                    ScanSubAnimationFolderForTags(subEntry.path(), subAnimDef);
                    modDef.subAnimations.push_back(subAnimDef);
                }
            }
            _allMods.push_back(modDef);
        }
    }

    // --- User Interface Logic ---
    void AnimationManager::DrawAddModModal() {
        if (_isAddModModalOpen) {
            if (_instanceToAddTo) {
                ImGui::OpenPopup(LOC("add_animation"));
            } else if (_modInstanceToAddTo || _userMovesetToAddTo || _stanceToAddTo) {
                ImGui::OpenPopup(LOC("add_moveset"));
            }
            _isAddModModalOpen = false;
        }
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 modal_list_size = ImVec2(viewport->Size.x * 0.5f, viewport->Size.y * 0.5f);
        ImVec2 center = ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f);
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        // Modal LOC("add_animation") (no changes, was already correct)
        if (ImGui::BeginPopupModal(LOC("add_animation"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text(LOC("library"));
            ImGui::Separator();
            ImGui::InputText(LOC("filter"), _movesetFilter, 128);
            if (ImGui::BeginChild("BibliotecaMovesets", ImVec2(modal_list_size), true)) {
                std::string filter_str = _movesetFilter;
                std::transform(filter_str.begin(), filter_str.end(), filter_str.begin(), ::tolower);
                for (size_t modIdx = 0; modIdx < _allMods.size(); ++modIdx) {
                    const auto& modDef = _allMods[modIdx];
                    std::string mod_name_str = modDef.name;
                    std::transform(mod_name_str.begin(), mod_name_str.end(), mod_name_str.begin(), ::tolower);
                    if (filter_str.empty() || mod_name_str.find(filter_str) != std::string::npos) {
                    
                        if (ImGui::Button((LOC("add") + modDef.name).c_str())) {
                            ModInstance newModInstance;
                            newModInstance.sourceModIndex = modIdx;
                            for (size_t subIdx = 0; subIdx < modDef.subAnimations.size(); ++subIdx) {
                                SubAnimationInstance newSubInstance;
                                newSubInstance.sourceModIndex = modIdx;
                                newSubInstance.sourceSubAnimIndex = subIdx;
                                newModInstance.subAnimationInstances.push_back(newSubInstance);
                            }
                            _instanceToAddTo->modInstances.push_back(newModInstance);
                        }
                        ImGui::SameLine(240);
                        ImGui::Text("%s", modDef.name.c_str());
                    
                    }
                }
            }
            ImGui::EndChild();
            if (ImGui::Button(LOC("close"))) {
                strcpy_s(_movesetFilter, "");
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Modal LOC("add_moveset") (WITH FIXES)
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal(LOC("add_moveset"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text(LOC("library"));
            ImGui::Separator();
            ImGui::InputText(LOC("filter"), _subMovesetFilter, 128);

            if (ImGui::BeginChild("BibliotecaSubMovesets", ImVec2(modal_list_size), true)) {
                std::string filter_str = _subMovesetFilter;
                std::transform(filter_str.begin(), filter_str.end(), filter_str.begin(), ::tolower);

                for (size_t modIdx = 0; modIdx < _allMods.size(); ++modIdx) {
                    const auto& modDef = _allMods[modIdx];
                    std::string mod_name_str = modDef.name;
                    std::transform(mod_name_str.begin(), mod_name_str.end(), mod_name_str.begin(), ::tolower);
                    if (filter_str.empty() || mod_name_str.find(filter_str) != std::string::npos) {
                        // If the parent passes the filter, show the TreeNode
                        if (ImGui::TreeNode(modDef.name.c_str())) {
                            // Inner loop over the sub-movesets (children)
                            for (size_t subAnimIdx = 0; subAnimIdx < modDef.subAnimations.size(); ++subAnimIdx) {
                                const auto& subAnimDef = modDef.subAnimations[subAnimIdx];

                                // NO FILTER IN HERE. Shows all children.

                                ImGui::PushID(static_cast<int>(modIdx * 1000 + subAnimIdx));

                                float button_width = 200.0f;
                                ImVec2 content_avail;
                                ImGui::GetContentRegionAvail(&content_avail);

                                if (ImGui::Button(LOC("add"))) {
                                    SubAnimationInstance newSubInstance;
                                    newSubInstance.sourceModIndex = modIdx;
                                    newSubInstance.sourceSubAnimIndex = subAnimIdx;
                                    const auto& sourceMod = _allMods[modIdx];
                                    const auto& sourceSubAnim = sourceMod.subAnimations[subAnimIdx];
                                    newSubInstance.sourceModName = sourceMod.name;
                                    newSubInstance.sourceSubName = sourceSubAnim.name;
                                    if (_modInstanceToAddTo) {
                                        _modInstanceToAddTo->subAnimationInstances.push_back(newSubInstance);
                                    } else if (_userMovesetToAddTo) {
                                        _userMovesetToAddTo->subAnimations.push_back(newSubInstance);
                                    } else if (_stanceToAddTo) {
                                        CreatorSubAnimationInstance newInstance;
                                        newInstance.sourceDef = &subAnimDef;
                                        strcpy_s(newInstance.editedName.data(), newInstance.editedName.size(),
                                                 subAnimDef.name.c_str());
                                        PopulateHkxFiles(newInstance);
                                        _stanceToAddTo->subMovesets.push_back(newInstance);
                                    }
                                }

                                if (content_avail.x > button_width) {
                                    ImGui::SameLine(button_width + 40);
                                } else {
                                    ImGui::SameLine();
                                }

                                ImGui::Text("%s", subAnimDef.name.c_str());
                                ImGui::PopID();
                            }
                            ImGui::TreePop();
                        }
                    }
                }
            }
            ImGui::EndChild();
            if (ImGui::Button(LOC("close"))) {
                strcpy_s(_subMovesetFilter, "");  // Clear the filter on close
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // This is the new main UI function that you will register in SKSEMenuFramework
    void AnimationManager::DrawMainMenu() {
        // First, draw the tab system
        if (ImGui::BeginTabBar("MainTabs")) {
            if (ImGui::BeginTabItem(LOC("tab_movesets"))) {
                DrawAnimationManager();  // Call the UI for the first tab
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(LOC("tab_moveset_creator"))) {
                DrawUserMovesetCreator();  // Call the new UI
                ImGui::EndTabItem();
            }
            //if (ImGui::BeginTabItem(LOC("tab_user_movesets"))) {
            //    DrawUserMovesetManager();  // Call the UI for the second tab
            //    ImGui::EndTabItem();
            //}
            if (ImGui::BeginTabItem(LOC("category_manager"))) {
                DrawCategoryManager();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        DrawAddModModal();
        DrawAddDarModal();
        DrawStanceEditorPopup();
        DrawRestartPopup();
        DrawCreateCategoryModal();
        
    }

    // New function to draw the moveset creation interface
    void AnimationManager::DrawUserMovesetCreator() {
        ImGui::Text("Moveset Creator");
        ImGui::Separator();

        // Main buttons section
        if (ImGui::Button(LOC("save"))) {
            SaveUserMoveset();
        }
        ImGui::SameLine();
        if (ImGui::Button("Read DAR animations")) {
            ScanDarAnimations();
        }
        ImGui::Separator();

        // Moveset Information Fields
        ImGui::InputText("Moveset Name", _newMovesetName, sizeof(_newMovesetName));
        ImGui::InputText("Author", _newMovesetAuthor, sizeof(_newMovesetAuthor));
        ImGui::InputText("Descripton", _newMovesetDesc, sizeof(_newMovesetDesc));

        ImGui::Separator();

        ImGui::Text("Select categories");
        ImGui::InputText(LOC("filter"), _categoryFilterBuffer, sizeof(_categoryFilterBuffer));
        if (ImGui::BeginChild("CategorySelector", ImVec2(0, 150), true)) {
            std::string filter_str = _categoryFilterBuffer;
            std::transform(filter_str.begin(), filter_str.end(), filter_str.begin(), ::tolower);

            for (const auto& pair : _categories) {
                std::string category_name_lower = pair.first;
                std::transform(category_name_lower.begin(), category_name_lower.end(), category_name_lower.begin(),
                               ::tolower);

                if (filter_str.empty() || category_name_lower.find(filter_str) != std::string::npos) {
                    if (_newMovesetCategorySelection.find(pair.first) == _newMovesetCategorySelection.end()) {
                        _newMovesetCategorySelection[pair.first] = false;
                    }
                    ImGui::Checkbox(pair.first.c_str(), &_newMovesetCategorySelection[pair.first]);
                }
            }
        }
        ImGui::EndChild();
        ImGui::Separator();

        ImGui::Text("Add animations");

        for (auto const& [categoryName, isSelected] : _newMovesetCategorySelection) {
            if (isSelected) {
                ImGui::PushID(categoryName.c_str());

                if (ImGui::CollapsingHeader(categoryName.c_str())) {
                    if (_movesetCreatorStances.find(categoryName) == _movesetCreatorStances.end()) {
                        _movesetCreatorStances[categoryName] = std::array<CreatorStance, 4>();
                    }
                    auto& stances = _movesetCreatorStances.at(categoryName);

                    if (ImGui::BeginTabBar(std::string("StanceTabs_" + categoryName).c_str())) {
                        for (int i = 0; i < 4; ++i) {
                            std::string stanceTabName = std::format("Stance {}", i + 1);
                            if (ImGui::BeginTabItem(stanceTabName.c_str())) {
                                if (ImGui::Button(std::format("Add animation to {}", i + 1).c_str())) {
                                    _isAddModModalOpen = true;
                                    _stanceToAddTo = &stances[i];
                                    _instanceToAddTo = nullptr;
                                    _modInstanceToAddTo = nullptr;
                                    _userMovesetToAddTo = nullptr;
                                }
                                ImGui::SameLine();
                                if (ImGui::Button(std::format("Add DAR animation to {}", i + 1).c_str())) {
                                    _isAddDarModalOpen = true;
                                    _stanceToAddTo = &stances[i];
                                    _instanceToAddTo = nullptr;
                                    _modInstanceToAddTo = nullptr;
                                    _userMovesetToAddTo = nullptr;
                                }
                                ImGui::Separator();

                                int subToRemove = -1;
                                int moveUpIndex = -1;
                                int moveDownIndex = -1;

                                for (size_t j = 0; j < stances[i].subMovesets.size(); ++j) {
                                    auto& subInst = stances[i].subMovesets[j];
                                    ImGui::PushID(static_cast<int>(j));

                                    // --- START OF ADDITION #2: REORDER BUTTONS ---
                                    if (j > 0) {
                                        if (ImGui::Button("Up")) {
                                            moveUpIndex = j;
                                        }
                                        ImGui::SameLine();
                                    }
                                    if (j < stances[i].subMovesets.size() - 1) {
                                        if (ImGui::Button("Down")) {
                                            moveDownIndex = j;
                                        }
                                        ImGui::SameLine();
                                    }
                                    // --- END OF ADDITION #2 ---

                                    if (ImGui::Button("X")) {
                                        subToRemove = j;
                                    }
                                    ImGui::SameLine();
                                    ImGui::InputText("##SubName", subInst.editedName.data(), subInst.editedName.size());
                                    ImGui::SameLine();
                                    ImGui::Text("<- %s", subInst.sourceDef->name.c_str());
                                    ImGui::SameLine();
                                    ImGui::Checkbox("ToBFCO", &subInst.isBFCO);

                                    ImGui::Indent();
                                    ImGui::Checkbox("F", &subInst.pFront);
                                    ImGui::SameLine();
                                    ImGui::Checkbox("FR", &subInst.pFrontRight);
                                    ImGui::SameLine();
                                    ImGui::Checkbox("FL", &subInst.pFrontLeft);
                                    ImGui::SameLine();
                                    ImGui::Checkbox("R", &subInst.pRight);
                                    ImGui::SameLine();
                                    ImGui::Checkbox("L", &subInst.pLeft);
                                    ImGui::SameLine();
                                    ImGui::Checkbox("B", &subInst.pBack);
                                    ImGui::SameLine();
                                    ImGui::Checkbox("BR", &subInst.pBackRight);
                                    ImGui::SameLine();
                                    ImGui::Checkbox("BL", &subInst.pBackLeft);
                                    //ImGui::SameLine();
                                    //ImGui::Checkbox("Rnd", &subInst.pRandom);
                                    //ImGui::SameLine();
                                    //ImGui::Checkbox("Movement", &subInst.pDodge);
                                    ImGui::Unindent();

                                    // Section to manage individual .hkx files
                                    if (!subInst.hkxFileSelection.empty()) {
                                        int selectedCount = 0;
                                        for (const auto& pair : subInst.hkxFileSelection) {
                                            if (pair.second) selectedCount++;
                                        }
                                        
                                        if (ImGui::CollapsingHeader("Manage Animation Files")) {
                                            ImGui::Indent();
                                            ImGui::TextDisabled("Deselect files you do not want to include:");
                                            if (ImGui::BeginChild("HkxFilesChild", ImVec2(0, 300), true)) {
                                                for (auto& [filename, isFileSelected] : subInst.hkxFileSelection) {
                                                    ImGui::Checkbox(filename.c_str(), &isFileSelected);
                                                }
                                            }
                                            ImGui::EndChild();
                                            ImGui::Unindent();
                                        }
                                    }

                                    ImGui::PopID();
                                    ImGui::Separator();
                                }

                                if (subToRemove != -1) {
                                    stances[i].subMovesets.erase(stances[i].subMovesets.begin() + subToRemove);
                                }
                                // --- START OF ADDITION #3: APPLIES THE ORDER CHANGE ---
                                if (moveUpIndex != -1) {
                                    std::swap(stances[i].subMovesets[moveUpIndex],
                                              stances[i].subMovesets[moveUpIndex - 1]);
                                }
                                if (moveDownIndex != -1) {
                                    std::swap(stances[i].subMovesets[moveDownIndex],
                                              stances[i].subMovesets[moveDownIndex + 1]);
                                }

                                ImGui::EndTabItem();
                            }
                        }
                        ImGui::EndTabBar();
                    }
                }
                ImGui::PopID();
            }
        }
    }

    void AnimationManager::DrawNPCMenu() { 
        DrawNPCManager();
        DrawAddModModal();
        DrawRestartPopup();
        DrawNpcSelectionModal();
    }

    int AnimationManager::GetMaxMovesetsFor(const std::string& category, int stanceIndex) { 
    
        if (stanceIndex < 0 || stanceIndex >= 4) {
            return 0;
        }
        // Look up the category in the map
        auto it = _maxMovesetsPerCategory.find(category);
        if (it != _maxMovesetsPerCategory.end()) {
            // If found, return the value for the specific stance
            return it->second[stanceIndex];
        }
        // If the category wasn't found, there are no movesets
        return 0;
    }

//int AnimationManager::GetMaxMovesetsForNPC(RE::Actor* actor, const std::string& category, int stanceIndex) {
//        // The stanceIndex check is kept for compatibility, although the new logic doesn't use it.
//        if (stanceIndex < 0 || stanceIndex >= 4) {
//            return 0;
//        }
//        if (!actor) {
//            return 0;
//        }
//        // 1. Try to find the Actor in the game from the given FormID.
//        // This is the crucial conversion step.
//        SKSE::log::info("[GetMaxMovesetsForNPC] Actor received. Calling FindBestMovesetConfiguration...");
//
//        // 2. Call our new main function to perform the search with fallback and priority.
//        NpcRuleMatch result = FindBestMovesetConfiguration(actor, category);
//
//        SKSE::log::info(
//            "[GetMaxMovesetsForNPC] Result received -> Count: {}, Priority: {}. Setting priority "
//            "variable.",
//            result.count, result.priority);
//
//        actor->SetGraphVariableInt("CycleMovesetNpcType", result.priority);
//
//        SKSE::log::info("[GetMaxMovesetsForNPC] Returning final count: {}", result.count);
//        // 4. Return the moveset count, fulfilling the original function's contract.
//        return result.count;
//    }

    const std::map<std::string, WeaponCategory>& AnimationManager::GetCategories() const { 
        return _categories; }

    void AnimationManager::DrawCategoryUI(WeaponCategory& category) {
        ImGui::PushID(category.name.c_str());
        if (ImGui::CollapsingHeader(category.name.c_str())) {
            ImGui::BeginGroup();
            if (ImGui::BeginTabBar(std::string("StanceTabs_" + category.name).c_str())) {
                for (int i = 0; i < 4; ++i) {
                    const char* currentStanceName = category.stanceNameBuffers[i].data();
                    bool tab_open = ImGui::BeginTabItem(currentStanceName);
                    if (tab_open) {
                        category.activeInstanceIndex = i;
                        CategoryInstance& instance = category.instances[i];

                        std::map<SubAnimationInstance*, int> playlistNumbers;
                        std::map<SubAnimationInstance*, int> parentNumbersForChildren;
                        int currentPlaylistCounter = 1;
                        int lastValidParentNumber = 0;
                        for (auto& modInst : instance.modInstances) {
                            if (!modInst.isSelected) continue;
                            for (auto& subInst : modInst.subAnimationInstances) {
                                if (!subInst.isSelected) continue;
                                bool isParent = !(subInst.pFront || subInst.pBack || subInst.pLeft || subInst.pRight ||
                                                  subInst.pFrontRight || subInst.pFrontLeft || subInst.pBackRight ||
                                                  subInst.pBackLeft || subInst.pRandom || subInst.pDodge);
                                if (isParent) {
                                    lastValidParentNumber = currentPlaylistCounter;
                                    playlistNumbers[&subInst] = currentPlaylistCounter;
                                    currentPlaylistCounter++;
                                } else {
                                    parentNumbersForChildren[&subInst] = lastValidParentNumber;
                                }
                            }
                        }

                        if (ImGui::Button(LOC("edit_stance_name"))) {
                            _isEditStanceModalOpen = true;
                            _categoryToEdit = &category;
                            _stanceIndexToEdit = i;
                            strcpy_s(_editStanceNameBuffer, sizeof(_editStanceNameBuffer), currentStanceName);
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(LOC("add_animation"))) {
                            _isAddModModalOpen = true;
                            _instanceToAddTo = &instance;
                            _modInstanceToAddTo = nullptr;
                        }
                        ImGui::Separator();

                        int modInstanceToRemove = -1;
                        for (size_t mod_i = 0; mod_i < instance.modInstances.size(); ++mod_i) {
                            auto& modInstance = instance.modInstances[mod_i];
                            const auto& sourceMod = _allMods[modInstance.sourceModIndex];

                            ImGui::PushID(static_cast<int>(mod_i));
                            const bool isParentDisabled = !modInstance.isSelected;
                            if (isParentDisabled) {
                                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle()->Colors[ImGuiCol_TextDisabled]);
                            }

                            if (ImGui::Button("X")) modInstanceToRemove = static_cast<int>(mod_i);
                            ImGui::SameLine();
                            ImGui::Checkbox("##modselect", &modInstance.isSelected);
                            ImGui::SameLine();
                            bool node_open = ImGui::TreeNode(sourceMod.name.c_str());

                            if (ImGui::BeginDragDropSource()) {
                                ImGui::SetDragDropPayload("DND_MOD_INSTANCE", &mod_i, sizeof(size_t));
                                ImGui::Text("Move moveset %s", sourceMod.name.c_str());
                                ImGui::EndDragDropSource();
                            }
                            if (ImGui::BeginDragDropTarget()) {
                                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DND_MOD_INSTANCE")) {
                                    size_t source_idx = *(const size_t*)payload->Data;
                                    std::swap(instance.modInstances[source_idx], instance.modInstances[mod_i]);
                                }
                            }

                            if (node_open) {
                                if (ImGui::Button(LOC("add_moveset"))) {
                                    _isAddModModalOpen = true;
                                    _modInstanceToAddTo = &modInstance;
                                    _instanceToAddTo = nullptr;
                                }

                                for (size_t sub_j = 0; sub_j < modInstance.subAnimationInstances.size(); ++sub_j) {
                                    auto& subInstance = modInstance.subAnimationInstances[sub_j];
                                    auto* currentSubInstancePtr = &subInstance;
                                    const auto& originMod = _allMods[subInstance.sourceModIndex];
                                    const auto& originSubAnim = originMod.subAnimations[subInstance.sourceSubAnimIndex];

                                    ImGui::PushID(static_cast<int>(sub_j));
                                    const bool isChildDisabled = !subInstance.isSelected || isParentDisabled;
                                    if (isChildDisabled) {
                                        ImGui::PushStyleColor(ImGuiCol_Text,
                                                              ImGui::GetStyle()->Colors[ImGuiCol_TextDisabled]);
                                    }

                                    ImGui::Separator();

                                    // --- Column 1 (Info) ---
                                    ImGui::BeginGroup();
                                    ImGui::Checkbox("##subselect", &subInstance.isSelected);
                                    ImGui::SameLine();

                                    ImGui::BeginGroup();

                                    ImVec2 selectableSize;
                                    ImVec2 contentRegionAvail;
                                    ImGui::GetContentRegionAvail(&contentRegionAvail);
                                    selectableSize.x = contentRegionAvail.x * 0.5f;  // Half of the remaining space
                                    selectableSize.y = ImGui::GetTextLineHeight();

                                    if (_subInstanceBeingEdited == currentSubInstancePtr) {
                                        ImGui::PushItemWidth(250);
                                        ImGui::SetKeyboardFocusHere();  // Automatic focus when entering edit mode
                                        if (ImGui::InputText("##SubAnimNameEdit", subInstance.editedName.data(),
                                                             subInstance.editedName.size(),
                                                             ImGuiInputTextFlags_EnterReturnsTrue |
                                                                 ImGuiInputTextFlags_AutoSelectAll)) {
                                            _subInstanceBeingEdited =
                                                nullptr;  // Exit edit mode when pressing Enter
                                        }
                                        // Exit edit mode if the field loses focus
                                        if (ImGui::IsItemDeactivatedAfterEdit()) {
                                            _subInstanceBeingEdited = nullptr;
                                        }
                                        ImGui::PopItemWidth();

                                    } else {
                                        // ============================ START OF FIX ============================

                                        // Determine which name to use: the edited one, or the original if the
                                        // edited one is empty.
                                        const char* displayName = (subInstance.editedName[0] != '\0')
                                                                      ? subInstance.editedName.data()
                                                                      : originSubAnim.name.c_str();

                                        // Build the label using the correct 'displayName'.
                                        std::string label = displayName;
                                        if (modInstance.isSelected && subInstance.isSelected) {
                                            if (playlistNumbers.count(&subInstance)) {
                                                label = std::format("[{}] {}", playlistNumbers.at(&subInstance),
                                                                    displayName);
                                            } else if (parentNumbersForChildren.count(&subInstance)) {
                                                label =
                                                    std::format(" -> [{}] {}",
                                                                parentNumbersForChildren.at(&subInstance), displayName);
                                            }
                                        }


                                        // Draw the selectable text
                                        ImGui::Selectable(label.c_str(), false, 0,
                                                          ImVec2(250, ImGui::GetTextLineHeight()));

                                        // EDIT TRIGGER IS NOW A CONTEXT MENU (RIGHT CLICK)
                                        if (ImGui::BeginPopupContextItem("sub_anim_context_menu")) {
                                            if (ImGui::MenuItem("Edit Name")) {
                                                _subInstanceBeingEdited =
                                                    currentSubInstancePtr;  // Activates edit mode
                                            }
                                            ImGui::EndPopup();
                                        }

                                        // DRAG AND DROP LOGIC (stays on the Selectable)
                                        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                                            ImGui::SetDragDropPayload("DND_SUB_INSTANCE", &sub_j, sizeof(size_t));
                                            ImGui::Text("Move %s", originSubAnim.name.c_str());
                                            ImGui::EndDragDropSource();
                                        }
                                    }

                                    // The Drag and Drop target can stay outside the if/else to always work
                                    if (ImGui::BeginDragDropTarget()) {
                                        if (const ImGuiPayload* payload =
                                                ImGui::AcceptDragDropPayload("DND_SUB_INSTANCE")) {
                                            size_t source_idx = *(const size_t*)payload->Data;
                                            std::swap(modInstance.subAnimationInstances[source_idx],
                                                      modInstance.subAnimationInstances[sub_j]);
                                        }
                                        ImGui::EndDragDropTarget();
                                    }

                                    // Tooltip with the original name (optional, but useful)
                                    if (ImGui::IsItemHovered()) {
                                        ImGui::SetTooltip(
                                            "Original: %s\nRight-click to edit name.\nDrag n Drop to move place",
                                            originSubAnim.name.c_str());
                                    }

                                    bool firstTag = true;
                                    if (originSubAnim.attackCount > 0) {
                                        if (!firstTag) ImGui::SameLine();
                                        ImGui::TextColored({1.0f, 0.4f, 0.4f, 1.0f}, "[HitCombo: %d]",
                                                           originSubAnim.attackCount);
                                        firstTag = false;
                                    }
                                    if (originSubAnim.powerAttackCount > 0) {
                                        if (!firstTag) ImGui::SameLine();
                                        ImGui::TextColored({1.0f, 0.6f, 0.2f, 1.0f}, "[PA: %d]",
                                                           originSubAnim.powerAttackCount);
                                        firstTag = false;
                                    }
                                    if (originSubAnim.hasIdle) {
                                        if (!firstTag) ImGui::SameLine();
                                        ImGui::TextColored({0.4f, 0.6f, 1.0f, 1.0f}, "[Idle]");
                                        firstTag = false;
                                    }

                                    ImGui::EndGroup();
                                    ImGui::EndGroup();

                                    ImGui::SameLine();

                                    // --- Column 2 (Checkboxes) ---
                                    ImGui::BeginGroup();

                                    struct CheckboxInfo {
                                        const char* label;
                                        bool* value;
                                    };
                                    std::vector<CheckboxInfo> checkboxes = {{"F", &subInstance.pFront},
                                                                            {"FR", &subInstance.pFrontRight},
                                                                            {"FL", &subInstance.pFrontLeft},
                                                                            {"R", &subInstance.pRight},
                                                                            {"L", &subInstance.pLeft},
                                                                            {"B", &subInstance.pBack},
                                                                            {"BR", &subInstance.pBackRight},
                                                                            {"BL", &subInstance.pBackLeft},
                                                                            //{"Rnd", &subInstance.pRandom},
                                                                            {"Movement", &subInstance.pDodge}};

                                    ImGui::GetContentRegionAvail(&contentRegionAvail);
                                    float availableWidth = contentRegionAvail.x;
                                    float currentX = 0.0f;
                                    float itemSpacing = ImGui::GetStyle()->ItemSpacing.x;
                                    float itemInnerSpacing = ImGui::GetStyle()->ItemInnerSpacing.x;

                                    for (size_t k = 0; k < checkboxes.size(); ++k) {
                                        const auto& cb = checkboxes[k];

                                        ImVec2 textSize;
                                        ImGui::CalcTextSize(&textSize, cb.label, NULL, false, 0.0f);

                                        float checkboxWidth = ImGui::GetFrameHeight() + itemInnerSpacing + textSize.x;

                                        if (k > 0) {
                                            if (currentX > 0.0f &&
                                                (currentX + itemSpacing + checkboxWidth) > availableWidth) {
                                                currentX = 0.0f;
                                            } else {
                                                ImGui::SameLine();
                                                currentX += itemSpacing;
                                            }
                                        }

                                        ImGui::Checkbox(cb.label, cb.value);
                                        currentX += checkboxWidth;
                                    }

                                    ImGui::EndGroup();

                                    if (isChildDisabled) {
                                        ImGui::PopStyleColor();
                                    }
                                    ImGui::PopID();
                                }
                                ImGui::TreePop();
                            }
                            if (isParentDisabled) {
                                ImGui::PopStyleColor();
                            }
                            ImGui::PopID();
                        }

                        if (modInstanceToRemove != -1) {
                            instance.modInstances.erase(instance.modInstances.begin() + modInstanceToRemove);
                        }
                        ImGui::EndTabItem();
                    }
                    
                }
                ImGui::EndTabBar();
            }
            ImGui::EndGroup();
        }
        ImGui::PopID();
    }

    void AnimationManager::DrawAnimationManager() {
        if (ImGui::Button(LOC("save"))) {
            SaveAllSettings();
        }
        ImGui::SameLine();
        ImGui::Checkbox(LOC("save_oldconditions"), &_preserveConditions);
        ImGui::Separator();

        // DrawAddModModal();

        if (_categories.empty()) {
            ImGui::Text("No animation category has been loaded.");
            return;
        }

        if(ImGui::BeginTabBar("WeaponTypeTabs")) {
            if (ImGui::BeginTabItem(LOC("tab_single_wield"))) {
                for (auto& pair : _categories) {
                    WeaponCategory& category = pair.second;
                    if (!category.isDualWield && !category.isShieldCategory) {  // Filter
                        DrawCategoryUI(pair.second);
                    }
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Dual-Wield")) {
                for (auto& pair : _categories) {
                    WeaponCategory& category = pair.second;
                    if (category.isDualWield) {  // Filter
                        DrawCategoryUI(pair.second);
                    }
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(LOC("tab_shield"))) {  
                for (auto& pair : _categories) {
                    WeaponCategory& category = pair.second;
                    if (category.isShieldCategory) {
                        DrawCategoryUI(pair.second);

                    }
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

    }

    void AnimationManager::DrawNPCManager() {
        // --- MODE SWITCH LOGIC (LIST vs. EDIT) ---
        if (_ruleToEdit != nullptr) {
            // --- EDIT MODE: Shows the editor for the selected rule ---

            if (ImGui::Button("Back")) {
                SKSE::log::info("[DrawNPCManager] 'Back' button clicked. Exiting edit mode.");
                _ruleToEdit = nullptr;  // Set to null to return to list mode
                return;
            }
            ImGui::SameLine();
            ImGui::TextDisabled(" | Editing rule: ");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s", _ruleToEdit->displayName.c_str());
            ImGui::Separator();

            // Get the category map for the rule we are editing
            auto& categoriesToDraw = _ruleToEdit->categories;

            if (categoriesToDraw.empty()) {
                ImGui::Text("This rule doesnt have categories");
            }

            // Reuse the same TabBar and UI logic you already had for NPCs
            if (ImGui::BeginTabBar("WeaponTypeTabs_NPC_Edit")) {
                if (ImGui::BeginTabItem(LOC("tab_single_wield"))) {
                    for (auto& pair : categoriesToDraw) {
                        if (!pair.second.isDualWield && !pair.second.isShieldCategory) {
                            DrawNPCCategoryUI(pair.second);  // Reuse the existing UI function!
                        }
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(LOC("tab_dual_wield"))) {
                    for (auto& pair : categoriesToDraw) {
                        if (pair.second.isDualWield) {
                            DrawNPCCategoryUI(pair.second);
                        }
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(LOC("tab_shield"))) {
                    for (auto& pair : categoriesToDraw) {
                        if (pair.second.isShieldCategory) {
                            DrawNPCCategoryUI(pair.second);
                        }
                    }
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }

        } else {
            // --- LIST MODE: Shows the filters and rule list (code from the previous step) ---
            //SKSE::log::info("[DrawNPCManager] Rendering in LIST MODE.");
            if (ImGui::Button(LOC("save"))) {
                SaveAllSettings();
            }
            ImGui::SameLine();
            if (ImGui::Button("Create new rule")) {
                _isCreateRuleModalOpen = true;
                ImGui::OpenPopup("Select Rule Type");
            }
            ImGui::Separator();

            ImGui::PushItemWidth(150);
            const char* filterTypes[] = {"All", "NPC", "Keyword", "Faction", "Race"};
            ImGui::Combo("Filter by type", &_ruleFilterType, filterTypes,
                         sizeof(filterTypes) / sizeof(filterTypes[0]));
            ImGui::PopItemWidth();

            ImGui::SameLine();
            ImGui::InputText("Search", _ruleFilterText, sizeof(_ruleFilterText));
            ImGui::Separator();

            if (ImGui::BeginTable("RulesTable", 4,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("Rule type", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Name / ID", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Plugin", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 180.0f);
                ImGui::TableHeadersRow();
                // --- Fixed row for the "NPCs (General)" rule ---
                ImGui::TableNextRow();

                // Column: Type
                ImGui::TableNextColumn();
                ImGui::TextDisabled("General");

                // Column: Name
                ImGui::TableNextColumn();
                ImGui::Text("NPCs (General)");
                ImGui::TextDisabled("Base rule for all NPCs that don't match a more specific rule.");

                // Column: Plugin
                ImGui::TableNextColumn();
                ImGui::Text("Plugin");

                // Column: Actions
                ImGui::TableNextColumn();
                ImGui::PushID("##GeneralRule");
                if (ImGui::Button("Edit")) {
                    SKSE::log::info("[DrawNPCManager] 'Edit' button for the General Rule clicked.");
                    _ruleToEdit = &_generalNpcRule;  // Points to our member variable
                }
                // No delete button here
                ImGui::PopID();
                std::string filterTextLower = _ruleFilterText;
                std::transform(filterTextLower.begin(), filterTextLower.end(), filterTextLower.begin(), ::tolower);

                for (auto it = _npcRules.begin(); it != _npcRules.end();) {
                    auto& rule = *it;
                    //SKSE::log::info("[DrawNPCManager] Processing rule in list: '{}'", rule.displayName);
                    bool skipRule = false;
                    if (_ruleFilterType != 0) {  // If not "All"
                        switch (_ruleFilterType) {
                            case 1:  // "NPC"
                                if (rule.type != RuleType::UniqueNPC) skipRule = true;
                                break;
                            case 2:  // "Keyword"
                                if (rule.type != RuleType::Keyword) skipRule = true;
                                break;
                            case 3:  // "Faction"
                                if (rule.type != RuleType::Faction) skipRule = true;
                                break;
                            case 4:  // "Race"
                                if (rule.type != RuleType::Race) skipRule = true;
                                break;
                            default:
                                // If any other value appears, don't filter anything
                                break;
                        }
                    }

                    if (skipRule) {
                        ++it;
                        continue;
                    }

                    std::string displayNameLower = rule.displayName;
                    std::transform(displayNameLower.begin(), displayNameLower.end(), displayNameLower.begin(),
                                   ::tolower);
                    if (!filterTextLower.empty() && displayNameLower.find(filterTextLower) == std::string::npos) {
                        ++it;
                        continue;
                    }

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const char* typeName = "Unknown";
                    switch (rule.type) {
                        case RuleType::UniqueNPC:
                            typeName = "NPC";
                            break;
                        case RuleType::Keyword:
                            typeName = "Keyword";
                            break;
                        case RuleType::Faction:
                            typeName = "Faction";
                            break;
                        case RuleType::Race:
                            typeName = "Race";
                            break;
                    }
                    ImGui::Text(typeName);

                    ImGui::TableNextColumn();
                    ImGui::Text("%s", rule.displayName.c_str());
                    ImGui::TextDisabled("%s", rule.identifier.c_str());

                    ImGui::TableNextColumn();
                    ImGui::Text("%s", rule.pluginName.c_str());

                    ImGui::TableNextColumn();
                    ImGui::PushID(&rule);
                    if (ImGui::Button("Edit")) {
                        SKSE::log::info("[DrawNPCManager] 'Edit' button clicked for rule: '{}'", rule.displayName);
                        _ruleToEdit = &rule;  // <-- THE MAGIC HAPPENS HERE!
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Delete")) {
                        it = _npcRules.erase(it);
                    } else {
                        ++it;
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
        // --- NEW POP-UP: Add this code block at the end of the function ---
        if (ImGui::BeginPopupModal("Select Rule Type", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Which rule type you want create?");
            ImGui::Separator();

            // Radio buttons to select the rule type
            // The _ruleTypeToCreate variable is the one we added to the .h earlier
            ImGui::RadioButton("NPC", reinterpret_cast<int*>(&_ruleTypeToCreate), (int)RuleType::UniqueNPC);
            ImGui::RadioButton("Keyword", reinterpret_cast<int*>(&_ruleTypeToCreate), (int)RuleType::Keyword);
            ImGui::RadioButton("Faction", reinterpret_cast<int*>(&_ruleTypeToCreate), (int)RuleType::Faction);
            ImGui::RadioButton("Race", reinterpret_cast<int*>(&_ruleTypeToCreate), (int)RuleType::Race);

            ImGui::Separator();

            if (ImGui::Button("Next", ImVec2(120, 0))) {
                // When the user clicks Next, we activate the main selection pop-up
                _isNpcSelectionModalOpen = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

    }

    // Helper for the NPC Category UI
    void AnimationManager::DrawNPCCategoryUI(WeaponCategory& category) {
        ImGui::PushID(category.name.c_str());
        if (ImGui::CollapsingHeader(category.name.c_str())) {
            // NPCs use instance 0 (Stance 0)
            CategoryInstance& instance = category.instances[0];

            // --- POINT 2: Logic to calculate the moveset order ---
            std::map<const SubAnimationInstance*, int> playlistNumbers;
            std::map<const SubAnimationInstance*, int> parentNumbersForChildren;
            int currentPlaylistCounter = 1;
            int lastValidParentNumber = 0;

            for (auto& modInst : instance.modInstances) {
                if (!modInst.isSelected) continue;
                for (auto& subInst : modInst.subAnimationInstances) {
                    if (!subInst.isSelected) continue;

                    bool isParent = !(subInst.pRandom || subInst.pDodge);

                    if (isParent) {
                        lastValidParentNumber = currentPlaylistCounter;
                        playlistNumbers[&subInst] = currentPlaylistCounter;
                        currentPlaylistCounter++;
                    } else {
                        parentNumbersForChildren[&subInst] = lastValidParentNumber;
                    }
                }
            }
            // --- END OF ORDER LOGIC ---

            if (ImGui::Button(LOC("add_animation"))) {
                _isAddModModalOpen = true;
                _instanceToAddTo = &instance;
                _modInstanceToAddTo = nullptr;
            }
            ImGui::Separator();

            int modInstanceToRemove = -1;
            for (size_t mod_i = 0; mod_i < instance.modInstances.size(); ++mod_i) {
                auto& modInstance = instance.modInstances[mod_i];
                const auto& sourceMod = _allMods[modInstance.sourceModIndex];

                ImGui::PushID(static_cast<int>(mod_i));

                const bool isParentDisabled = !modInstance.isSelected;
                if (isParentDisabled) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle()->Colors[ImGuiCol_TextDisabled]);
                }

                ImGui::Columns(2, std::string("mod_instance_columns_" + std::to_string(mod_i)).c_str(), false);

                ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() * 0.57f);  // Column 1 uses 60% of the space

                // --- COLUMN 1: Moveset Controls and Name ---
                if (ImGui::Button("X")) modInstanceToRemove = static_cast<int>(mod_i);
                ImGui::SameLine();
                ImGui::Checkbox("##modselect", &modInstance.isSelected);
                ImGui::SameLine();

                // The TreeNode is now inside a column, so its width is limited.
                bool node_open = ImGui::TreeNode(sourceMod.name.c_str());

                // The Drag & Drop logic now applies to the TreeNode, but the area is limited by the column.
                if (ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("DND_MOD_INSTANCE_NPC", &mod_i, sizeof(size_t));
                    ImGui::Text("Move moveset %s", sourceMod.name.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DND_MOD_INSTANCE_NPC")) {
                        size_t source_idx = *(const size_t*)payload->Data;
                        if (source_idx != mod_i) {
                            std::swap(instance.modInstances[source_idx], instance.modInstances[mod_i]);
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                ImGui::NextColumn();  // Move to the next column

                if (_instanceBeingEdited == &modInstance) {
                    // EDIT MODE: Shows the Input fields
                    ImGui::PushItemWidth(60);  // Sets a small width for each field
                    ImGui::InputInt("Hp", &modInstance.hp, 0);
                    ImGui::SameLine();
                    ImGui::InputInt("St", &modInstance.st, 0);
                    ImGui::SameLine();
                    ImGui::InputInt("Mn", &modInstance.mn, 0);
                    ImGui::SameLine();
                    ImGui::InputInt("Lv", &modInstance.level, 0);
                    ImGui::SameLine();
                    ImGui::PopItemWidth();

                    // Button to save and exit edit mode
                    if (ImGui::Button("OK")) {
                        // Optional data validation before saving
                        modInstance.hp = std::clamp(modInstance.hp, 0, 100);
                        modInstance.st = std::clamp(modInstance.st, 0, 100);
                        modInstance.mn = std::clamp(modInstance.mn, 0, 100);
                        if (modInstance.level < 0) modInstance.level = 0;

                        _instanceBeingEdited = nullptr;  // Exit edit mode
                    }

                } else {
                    // VIEW MODE: Shows the text
                    std::string conditions_text =
                        std::format("Hp <= {}% | St <= {}% | Mn <= {}% | Lv => {}", modInstance.hp, modInstance.st,
                                    modInstance.mn, modInstance.level);

                    ImGui::Selectable(conditions_text.c_str(), false, 0, ImVec2(0, ImGui::GetTextLineHeight()));

                    // The context menu ACTIVATES edit mode
                    if (ImGui::BeginPopupContextItem("condition_context_menu")) {
                        if (ImGui::MenuItem("Edit Conditions")) {
                            _instanceBeingEdited = &modInstance;
                        }
                        ImGui::EndPopup();
                    }

                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Right-click to edit conditions");
                    }
                }
                ImGui::Columns(1);

                if (node_open) {
                    if (ImGui::Button(LOC("add_moveset"))) {
                        _isAddModModalOpen = true;           // Opens the modal menu
                        _modInstanceToAddTo = &modInstance;  // Points to the current NPC moveset
                        _instanceToAddTo = nullptr;          // Clears the general instance pointer
                        _userMovesetToAddTo = nullptr;       // Clears the user moveset pointer
                    }
                    for (size_t sub_j = 0; sub_j < modInstance.subAnimationInstances.size(); ++sub_j) {
                        auto& subInstance = modInstance.subAnimationInstances[sub_j];
                        const auto& originMod = _allMods[subInstance.sourceModIndex];
                        const auto& originSubAnim = originMod.subAnimations[subInstance.sourceSubAnimIndex];

                        ImGui::PushID(static_cast<int>(sub_j));

                        const bool isChildDisabled = !subInstance.isSelected || isParentDisabled;
                        if (isChildDisabled) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle()->Colors[ImGuiCol_TextDisabled]);
                        }
                        ImGui::BeginGroup();

                        ImGui::Checkbox("##subselect", &subInstance.isSelected);
                        ImGui::SameLine();

                        // Label that will be the drag area
                        std::string label;
                        if (modInstance.isSelected && subInstance.isSelected) {
                            if (playlistNumbers.count(&subInstance)) {
                                label = std::format("[{}] {}", playlistNumbers.at(&subInstance), originSubAnim.name);
                            } else if (parentNumbersForChildren.count(&subInstance)) {
                                int parentNum = parentNumbersForChildren.at(&subInstance);
                                label = std::format(" -> [{}] {}", parentNum, originSubAnim.name);
                            } else {
                                label = originSubAnim.name;  // Fallback
                            }
                        } else {
                            label = originSubAnim.name;  // Shows plain name if unchecked
                        }

                        // KEY POINT: We create a Selectable with a defined size.
                        // This restricts the drag-and-drop area, leaving space for other widgets.
                        ImVec2 contentRegionAvail;
                        ImGui::GetContentRegionAvail(&contentRegionAvail);
                        ImVec2 selectableSize(contentRegionAvail.x * 0.7f,
                                              ImGui::GetTextLineHeight());  // Takes up 70% of the remaining space

                        ImGui::Selectable(label.c_str(), false, 0, selectableSize);

                        // We apply Drag & Drop ONLY to the Selectable we just created.
                        if (ImGui::BeginDragDropSource()) {
                            ImGui::SetDragDropPayload("DND_SUB_INSTANCE_NPC", &sub_j, sizeof(size_t));
                            ImGui::Text("Move %s", originSubAnim.name.c_str());
                            ImGui::EndDragDropSource();
                        }
                        if (ImGui::BeginDragDropTarget()) {
                            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DND_SUB_INSTANCE_NPC")) {
                                size_t source_idx = *(const size_t*)payload->Data;
                                if (source_idx != sub_j) {
                                    std::swap(modInstance.subAnimationInstances[source_idx],
                                              modInstance.subAnimationInstances[sub_j]);
                                }
                            }
                            ImGui::EndDragDropTarget();
                        }

                        ImGui::EndGroup();  // End of Column 1

                        // --- Column 2: Property checkboxes ---
                        ImGui::SameLine();  // Moves the cursor to the same line, next to Column 1

                        ImGui::BeginGroup();  // Groups the checkboxes to ensure alignment

                        // Your checkboxes are now outside the drag area and are clickable.
                        // ImGui::Checkbox("Rnd", &subInstance.pRandom);
                        // ImGui::SameLine(); // If there is more than one, use SameLine
                        ImGui::Checkbox("Movement", &subInstance.pDodge);

                        ImGui::EndGroup();  // End of Column 2
                        // --- END OF CHECKBOXES ---

                        if (isChildDisabled) {
                            ImGui::PopStyleColor();
                        }

                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }

                if (isParentDisabled) {
                    ImGui::PopStyleColor();
                }
                ImGui::PopID();
            }

            if (modInstanceToRemove != -1) {
                instance.modInstances.erase(instance.modInstances.begin() + modInstanceToRemove);
            }
        }
        ImGui::PopID();
}

    void AnimationManager::SaveAllSettings() {
        SKSE::log::info("Starting global save of all settings...");
        SaveCustomCategories();
        SaveStanceNames();
        SaveCycleMovesets();  // This one has already been fixed and is working.
        SKSE::log::info("Generating condition files for OAR...");
        std::map<std::filesystem::path, std::vector<FileSaveConfig>> fileUpdates;

        auto processCategoriesForOAR = [&](const std::map<std::string, WeaponCategory>& sourceCategories,
                                           const MovesetRule* rule = nullptr) {
            bool isNpcRule = (rule != nullptr);  // Determines whether this is an NPC rule or the Player

            for (const auto& pair : sourceCategories) {
                const WeaponCategory& category = pair.second;

                // --- FIXED LOGIC: Determines how many stances to iterate ---
                int maxStances = isNpcRule ? 1 : 4;  // NPCs use 1 stance (index 0), Player uses 4.

                for (int i = 0; i < maxStances; ++i) {
                    const CategoryInstance& instance = category.instances[i];

                    // ========================= START OF FIX =========================
                    // STEP 1: PRE-PROCESSING
                    // Maps the 'order_in_playlist' of a parent to a set of its children's directions.
                    std::map<int, std::set<int>> childDirectionsByParentOrder;
                    int tempPlaylistParentCounter = 1;
                    int tempLastParentOrder = 0;

                    for (const auto& modInst : instance.modInstances) {
                        if (!modInst.isSelected) continue;
                        for (const auto& subInst : modInst.subAnimationInstances) {
                            if (!subInst.isSelected) continue;

                            bool isParent = !(subInst.pFront || subInst.pBack || subInst.pLeft || subInst.pRight ||
                                              subInst.pFrontRight || subInst.pFrontLeft || subInst.pBackRight ||
                                              subInst.pBackLeft || subInst.pRandom || subInst.pDodge);

                            if (isParent) {
                                tempLastParentOrder = tempPlaylistParentCounter++;
                            } else {
                                if (tempLastParentOrder > 0) {  // Ensures there is a parent to associate with
                                    if (subInst.pFront) childDirectionsByParentOrder[tempLastParentOrder].insert(1);
                                    if (subInst.pFrontRight)
                                        childDirectionsByParentOrder[tempLastParentOrder].insert(2);
                                    if (subInst.pRight) childDirectionsByParentOrder[tempLastParentOrder].insert(3);
                                    if (subInst.pBackRight) childDirectionsByParentOrder[tempLastParentOrder].insert(4);
                                    if (subInst.pBack) childDirectionsByParentOrder[tempLastParentOrder].insert(5);
                                    if (subInst.pBackLeft) childDirectionsByParentOrder[tempLastParentOrder].insert(6);
                                    if (subInst.pLeft) childDirectionsByParentOrder[tempLastParentOrder].insert(7);
                                    if (subInst.pFrontLeft) childDirectionsByParentOrder[tempLastParentOrder].insert(8);
                                }
                            }
                        }
                    }
                    // =======================================================================

                    int playlistParentCounter = 1;
                    int lastParentOrder = 0;

                    for (const auto& modInst : instance.modInstances) {
                        if (!modInst.isSelected) continue;

                        for (const auto& subInst : modInst.subAnimationInstances) {
                            if (!subInst.isSelected) continue;
                            const auto& sourceMod = _allMods[subInst.sourceModIndex];
                            const auto& sourceSubAnim =
                                _allMods[subInst.sourceModIndex].subAnimations[subInst.sourceSubAnimIndex];

                            FileSaveConfig config;

                            if (rule) {
                                config.ruleType = rule->type;
                                config.formID = rule->formID;
                                config.pluginName = rule->pluginName;
                                config.ruleIdentifier = rule->identifier;
                            } else {
                                config.ruleType = RuleType::Player;
                                config.formID = 0x7;
                                config.pluginName = "Skyrim.esm";
                                config.ruleIdentifier = "Player";
                            }

                            config.category = &category;
                            config.instance_index = isNpcRule ? 0 : i + 1;
                            config.pFront = subInst.pFront;
                            config.pBack = subInst.pBack;
                            config.pLeft = subInst.pLeft;
                            config.pRight = subInst.pRight;
                            config.pFrontRight = subInst.pFrontRight;
                            config.pFrontLeft = subInst.pFrontLeft;
                            config.pBackRight = subInst.pBackRight;
                            config.pBackLeft = subInst.pBackLeft;
                            config.pRandom = subInst.pRandom;
                            config.pDodge = subInst.pDodge;

                            bool isParent = !(config.pFront || config.pBack || config.pLeft || config.pRight ||
                                              config.pFrontRight || config.pFrontLeft || config.pBackRight ||
                                              config.pBackLeft || config.pRandom || config.pDodge);

                            config.isParent = isParent;

                            if (isParent) {
                                lastParentOrder = playlistParentCounter;
                                config.order_in_playlist = playlistParentCounter++;

                                // STEP 2: POPULATE THE childDirections FIELD USING THE MAP
                                auto it = childDirectionsByParentOrder.find(config.order_in_playlist);
                                if (it != childDirectionsByParentOrder.end()) {
                                    config.childDirections = it->second;
                                }
                                // ======================= END OF FIX =======================

                            } else {
                                config.order_in_playlist = lastParentOrder;
                            }
                            std::filesystem::path configPath;
                            if (sourceMod.name == "[DAR] Animations") {
                                // For DAR, the sub-animation's 'path' is the directory.
                                // We create a logical path to a config.json inside it
                                // so that UpdateOrCreateJson can find the parent directory correctly.
                                configPath = sourceSubAnim.path / "user.json";
                            } else {
                                // For OAR, the path is already the config.json file.
                                configPath = sourceSubAnim.path;
                            }
                            fileUpdates[configPath].push_back(config);
                        }
                    }
                }
            }
        };

        // 2. Collect the settings from all rule sources
        SKSE::log::info("Collecting Player settings...");
        processCategoriesForOAR(_categories);
        SKSE::log::info("Collecting General NPC settings...");
        processCategoriesForOAR(_generalNpcRule.categories, &_generalNpcRule);
        SKSE::log::info("Collecting settings for {} specific rules...", _npcRules.size());
        for (const auto& specificRule : _npcRules) {
            processCategoriesForOAR(specificRule.categories, &specificRule);
        }

        // 3. Clean up managed files that are no longer in use
        for (const auto& managedPath : _managedFiles) {
            if (fileUpdates.find(managedPath) == fileUpdates.end()) {
                fileUpdates[managedPath] = {};  // Add to the deactivation queue
            }
        }
        for (const auto& pair : fileUpdates) {
            _managedFiles.insert(pair.first);
        }

        // 4. Write the config.json files using the updated UpdateOrCreateJson logic
        SKSE::log::info("{} OAR configuration files will be modified.", fileUpdates.size());
        for (const auto& updateEntry : fileUpdates) {
            UpdateOrCreateJson(updateEntry.first, updateEntry.second);
        }

        SKSE::log::info("Global save complete.");
        RE::DebugNotification("All settings have been saved!");
        UpdateMaxMovesetCache();
        _showRestartPopup = true;
}


    void AnimationManager::UpdateOrCreateJson(const std::filesystem::path& jsonPath,
                                              const std::vector<FileSaveConfig>& configs) {
        rapidjson::Document doc;
        std::ifstream fileStream(jsonPath);
        if (fileStream) {
            std::string jsonContent((std::istreambuf_iterator<char>(fileStream)), std::istreambuf_iterator<char>());
            fileStream.close();
            if (doc.Parse(jsonContent.c_str()).HasParseError()) {
                SKSE::log::error("Parse error reading {}. Creating a new file.", jsonPath.string());
                doc.SetObject();
            }
        } else {
            doc.SetObject();
        }

        if (!doc.IsObject()) doc.SetObject();
        auto& allocator = doc.GetAllocator();

        std::string movesetName = jsonPath.parent_path().filename().string();
        if (doc.HasMember("name")) {
            doc["name"].SetString(movesetName.c_str(), allocator);
        } else {
            doc.AddMember("name", rapidjson::Value(movesetName.c_str(), allocator), allocator);
        }

        int basePriority = 2100000000;
        bool isUsedAsParent = false;
        for (const auto& config : configs) {
            if (config.isParent) {
                isUsedAsParent = true;
                break;
            }
        }
        int finalPriority = isUsedAsParent ? basePriority : basePriority + 1;

        if (doc.HasMember("priority")) {
            doc["priority"].SetInt(finalPriority);
        } else {
            doc.AddMember("priority", finalPriority, allocator);
        }

        rapidjson::Value oldConditions(rapidjson::kArrayType);
        if (_preserveConditions && doc.HasMember("conditions") && doc["conditions"].IsArray()) {
            for (auto& cond : doc["conditions"].GetArray()) {
                if (cond.IsObject() && cond.HasMember("comment") && cond["comment"] == "OAR_CYCLE_MANAGER_CONDITIONS") {
                    continue;
                }
                rapidjson::Value c;
                c.CopyFrom(cond, allocator);
                oldConditions.PushBack(c, allocator);
            }
        }

        if (doc.HasMember("conditions")) {
            doc["conditions"].SetArray();
        } else {
            doc.AddMember("conditions", rapidjson::Value(rapidjson::kArrayType), allocator);
        }
        rapidjson::Value& conditions = doc["conditions"];

        if (_preserveConditions && !oldConditions.Empty()) {
            rapidjson::Value oldConditionsBlock(rapidjson::kObjectType);
            oldConditionsBlock.AddMember("condition", "OR", allocator);
            oldConditionsBlock.AddMember("comment", "Old Conditions", allocator);
            oldConditionsBlock.AddMember("Conditions", oldConditions, allocator);
            conditions.PushBack(oldConditionsBlock, allocator);
        }

        /*std::map<int, std::set<int>> childDirectionsByPlaylist;
        for (const auto& config : configs) {
            if (!config.isParent) {
                int playlistId = config.order_in_playlist;
                if (playlistId > 0) {
                    if (config.pFront) childDirectionsByPlaylist[playlistId].insert(1);
                    if (config.pFrontRight) childDirectionsByPlaylist[playlistId].insert(2);
                    if (config.pRight) childDirectionsByPlaylist[playlistId].insert(3);
                    if (config.pBackRight) childDirectionsByPlaylist[playlistId].insert(4);
                    if (config.pBack) childDirectionsByPlaylist[playlistId].insert(5);
                    if (config.pBackLeft) childDirectionsByPlaylist[playlistId].insert(6);
                    if (config.pLeft) childDirectionsByPlaylist[playlistId].insert(7);
                    if (config.pFrontLeft) childDirectionsByPlaylist[playlistId].insert(8);
                }
            }
        }*/

        if (!configs.empty()) {
            rapidjson::Value masterOrBlock(rapidjson::kObjectType);
            masterOrBlock.AddMember("condition", "OR", allocator);
            masterOrBlock.AddMember("comment", "OAR_CYCLE_MANAGER_CONDITIONS", allocator);
            rapidjson::Value innerConditions(rapidjson::kArrayType);

            for (const auto& config : configs) {
                rapidjson::Value categoryAndBlock(rapidjson::kObjectType);
                categoryAndBlock.AddMember("condition", "AND", allocator);
                rapidjson::Value andConditions(rapidjson::kArrayType);

                // ActorBase condition
                switch (config.ruleType) {
                    case RuleType::Player:
                        AddIsActorBaseCondition(andConditions, "Skyrim.esm", 0x7, false, allocator);
                        break;
                    case RuleType::GeneralNPC:
                        AddIsActorBaseCondition(andConditions, "Skyrim.esm", 0x7, true, allocator);
                        break;
                    case RuleType::UniqueNPC:
                        AddIsActorBaseCondition(andConditions, config.pluginName, config.formID, false, allocator);
                        break;
                    case RuleType::Faction:
                        AddIsInFactionCondition(andConditions, config.pluginName, config.formID, allocator);
                        break;
                    case RuleType::Keyword:
                        AddHasKeywordCondition(andConditions, config.pluginName, config.formID, allocator);
                        break;
                    case RuleType::Race:
                        AddIsRaceCondition(andConditions, config.pluginName, config.formID, allocator);
                        break;
                }

                // NPC Type condition
                int priorityValue = GetPriorityForType(config.ruleType);
                AddCompareValuesCondition(andConditions, "CycleMovesetNpcType", priorityValue, allocator);

                // Right-Hand Equipped Type condition
                {
                    if (config.category->equippedTypeValue < 0.0) {  // Generic category like "Shield"
                        rapidjson::Value rightHandAndBlock(rapidjson::kObjectType);
                        rightHandAndBlock.AddMember("condition", "AND", allocator);
                        rapidjson::Value conditionsForRightHand(rapidjson::kArrayType);

                        rapidjson::Value orBlock(rapidjson::kObjectType);
                        orBlock.AddMember("condition", "OR", allocator);
                        rapidjson::Value orConditions(rapidjson::kArrayType);
                        AddCompareEquippedTypeCondition(orConditions, 1.0, false, allocator);
                        AddCompareEquippedTypeCondition(orConditions, 2.0, false, allocator);
                        AddCompareEquippedTypeCondition(orConditions, 3.0, false, allocator);
                        AddCompareEquippedTypeCondition(orConditions, 4.0, false, allocator);
                        orBlock.AddMember("Conditions", orConditions, allocator);
                        conditionsForRightHand.PushBack(orBlock, allocator);

                        // FIX #2: The exclusion logic for the base Shield category is now called correctly.
                        AddShieldCategoryExclusions(conditionsForRightHand, allocator);

                        rightHandAndBlock.AddMember("Conditions", conditionsForRightHand, allocator);
                        andConditions.PushBack(rightHandAndBlock, allocator);
                    } else {  // Default case
                        AddCompareEquippedTypeCondition(andConditions, config.category->equippedTypeValue, false,
                                                        allocator);
                    }
                }

                // Right-Hand Keyword conditions
                AddKeywordOrConditions(andConditions, config.category->keywords, false, allocator);
                AddCompetingKeywordExclusions(andConditions, config.category, false, allocator);

                // Left-Hand Keyword conditions
                if (!config.category->leftHandKeywords.empty()) {
                    AddKeywordOrConditions(andConditions, config.category->leftHandKeywords, true, allocator);
                    AddCompetingKeywordExclusions(andConditions, config.category, true, allocator);
                }

                // Left-Hand Equipped Type condition
                if (config.category->leftHandEquippedTypeValue >= 0.0) {
                    AddCompareEquippedTypeCondition(andConditions, config.category->leftHandEquippedTypeValue, true,
                                                    allocator);

                    // FIX #2: The block that caused keyword duplication was removed from here.
                    // The left-hand keyword check is already done above.
                }

                // ADDITION: Safety fix for the player instance
                int final_instance_index = config.instance_index;
                // If the config is for the player (!isNPC) and the index is invalid (< 1), fix it to 1.
                if (config.ruleType == RuleType::Player && final_instance_index < 1) {
                    SKSE::log::warn(
                        "Invalid instance index (0) found for the Player in {}. Fixing to 1.",
                        jsonPath.string());
                    final_instance_index = 1;  // Ensures the minimum value for the player is 1
                    
                }

                if (config.ruleType != RuleType::Player) {
                    final_instance_index = 0;
                }
                AddCompareValuesCondition(andConditions, "cycle_instance", final_instance_index, allocator);
                // Stance and Playlist order
                
                if (config.order_in_playlist > 0) {
                    AddCompareValuesCondition(andConditions, "testarone", config.order_in_playlist, allocator);
                    if (config.isParent) {
                        // Access the new member directly from the config object!
                        const auto& childDirs = config.childDirections;
                        if (!childDirs.empty()) {

                            // 1. Create a new AND block to group the negated conditions
                            rapidjson::Value negatedAndBlock(rapidjson::kObjectType);
                            negatedAndBlock.AddMember("condition", "AND", allocator);
                            negatedAndBlock.AddMember("comment", "Is NOT any child direction", allocator);

                            // 2. Create an array for the conditions inside this new block
                            rapidjson::Value innerNegatedConditions(rapidjson::kArrayType);

                            // 3. Add all negated conditions to THIS NEW ARRAY
                            for (int dirValue : childDirs) {
                                AddNegatedCompareValuesCondition(innerNegatedConditions, "DirecionalCycleMoveset",
                                                                 dirValue, allocator);
                            }

                            // 4. Attach the conditions array to the new AND block
                            negatedAndBlock.AddMember("Conditions", innerNegatedConditions, allocator);

                            // 5. Add the AND block (containing all negations) to the main array
                            andConditions.PushBack(negatedAndBlock, allocator);
                        }
                    } else {
                        if (config.pRandom) {
                            AddRandomCondition(andConditions, config.order_in_playlist, allocator);
                        }
                        rapidjson::Value directionalOrConditions(rapidjson::kArrayType);
                        if (config.pFront)
                            AddCompareValuesCondition(directionalOrConditions, "DirecionalCycleMoveset", 1, allocator);
                        if (config.pFrontRight)
                            AddCompareValuesCondition(directionalOrConditions, "DirecionalCycleMoveset", 2, allocator);
                        if (config.pRight)
                            AddCompareValuesCondition(directionalOrConditions, "DirecionalCycleMoveset", 3, allocator);
                        if (config.pBackRight)
                            AddCompareValuesCondition(directionalOrConditions, "DirecionalCycleMoveset", 4, allocator);
                        if (config.pBack)
                            AddCompareValuesCondition(directionalOrConditions, "DirecionalCycleMoveset", 5, allocator);
                        if (config.pBackLeft)
                            AddCompareValuesCondition(directionalOrConditions, "DirecionalCycleMoveset", 6, allocator);
                        if (config.pLeft)
                            AddCompareValuesCondition(directionalOrConditions, "DirecionalCycleMoveset", 7, allocator);
                        if (config.pFrontLeft)
                            AddCompareValuesCondition(directionalOrConditions, "DirecionalCycleMoveset", 8, allocator);
                        if (!directionalOrConditions.Empty()) {
                            rapidjson::Value orBlock(rapidjson::kObjectType);
                            orBlock.AddMember("condition", "OR", allocator);
                            orBlock.AddMember("Conditions", directionalOrConditions, allocator);
                            andConditions.PushBack(orBlock, allocator);
                        }
                    }
                }

                categoryAndBlock.AddMember("Conditions", andConditions, allocator);
                innerConditions.PushBack(categoryAndBlock, allocator);
            }
            if (!innerConditions.Empty()) {
                masterOrBlock.AddMember("Conditions", innerConditions, allocator);
                conditions.PushBack(masterOrBlock, allocator);
            }
        } else {  // "Kill switch" condition
            rapidjson::Value masterOrBlock(rapidjson::kObjectType);
            masterOrBlock.AddMember("condition", "OR", allocator);
            masterOrBlock.AddMember("comment", "OAR_CYCLE_MANAGER_CONDITIONS", allocator);
            rapidjson::Value innerConditions(rapidjson::kArrayType);
            rapidjson::Value andBlock(rapidjson::kObjectType);
            andBlock.AddMember("condition", "AND", allocator);
            rapidjson::Value andConditions(rapidjson::kArrayType);
            AddCompareValuesCondition(andConditions, "CycleMovesetDisable", 1, allocator);
            andBlock.AddMember("Conditions", andConditions, allocator);
            innerConditions.PushBack(andBlock, allocator);
            masterOrBlock.AddMember("Conditions", innerConditions, allocator);
            conditions.PushBack(masterOrBlock, allocator);
        }

        // Save the document
        FILE* fp;
        fopen_s(&fp, jsonPath.string().c_str(), "wb");
        if (!fp) {
            SKSE::log::error("Failed to open file for writing: {}", jsonPath.string());
            return;
        }
        char writeBuffer[65536];
        rapidjson::FileWriteStream os(fp, writeBuffer, sizeof(writeBuffer));
        rapidjson::PrettyWriter<rapidjson::FileWriteStream> writer(os);
        doc.Accept(writer);
        fclose(fp);
    }


    void AnimationManager::AddCompareValuesCondition(rapidjson::Value& conditionsArray, const std::string& graphVarName,
                                                     int value, rapidjson::Document::AllocatorType& allocator) {
        rapidjson::Value newCompare(rapidjson::kObjectType);
        newCompare.AddMember("condition", "CompareValues", allocator);
        newCompare.AddMember("requiredVersion", "1.0.0.0", allocator);
        rapidjson::Value valueA(rapidjson::kObjectType);
        valueA.AddMember("value", value, allocator);  
        newCompare.AddMember("Value A", valueA, allocator);
        newCompare.AddMember("Comparison", "==", allocator);
        rapidjson::Value valueB(rapidjson::kObjectType);
        valueB.AddMember("graphVariable", rapidjson::Value(graphVarName.c_str(), allocator), allocator);
        valueB.AddMember("graphVariableType", "Int", allocator);
        newCompare.AddMember("Value B", valueB, allocator);
        conditionsArray.PushBack(newCompare, allocator);
    }

    // NEW HELPER FUNCTION: Adds a "CompareValues" condition for a boolean value.
    // Used to check the movement checkboxes (F, B, L, R, etc.).
    void AnimationManager::AddCompareBoolCondition(rapidjson::Value& conditionsArray, const std::string& graphVarName,
                                                   bool value, rapidjson::Document::AllocatorType& allocator) {
        rapidjson::Value newCompare(rapidjson::kObjectType);
        newCompare.AddMember("condition", "CompareValues", allocator);
        newCompare.AddMember("requiredVersion", "1.0.0.0", allocator);

        rapidjson::Value valueA(rapidjson::kObjectType);
        valueA.AddMember("value", value, allocator);
        newCompare.AddMember("Value A", valueA, allocator);

        newCompare.AddMember("Comparison", "==", allocator);

        rapidjson::Value valueB(rapidjson::kObjectType);
        valueB.AddMember("graphVariable", rapidjson::Value(graphVarName.c_str(), allocator), allocator);
        valueB.AddMember("graphVariableType", "bool", allocator);  // The type here is "bool"
        newCompare.AddMember("Value B", valueB, allocator);

        conditionsArray.PushBack(newCompare, allocator);
    }

    void AnimationManager::AddRandomCondition(rapidjson::Value& conditionsArray, int value,
                                              rapidjson::Document::AllocatorType& allocator) {
        rapidjson::Value newRandom(rapidjson::kObjectType);
        newRandom.AddMember("condition", "Random", allocator);
        newRandom.AddMember("requiredVersion", "2.3.0.0", allocator);

        rapidjson::Value state(rapidjson::kObjectType);
        state.AddMember("scope", "Local", allocator);
        state.AddMember("shouldResetOnLoopOrEcho", true, allocator);
        newRandom.AddMember("State", state, allocator);

        rapidjson::Value minVal(rapidjson::kObjectType);
        minVal.AddMember("value", static_cast<double>(value), allocator);
        newRandom.AddMember("Minimum random value", minVal, allocator);

        rapidjson::Value maxVal(rapidjson::kObjectType);
        maxVal.AddMember("value", static_cast<double>(value), allocator);
        newRandom.AddMember("Maximum random value", maxVal, allocator);

        newRandom.AddMember("Comparison", "==", allocator);

        rapidjson::Value numVal(rapidjson::kObjectType);
        numVal.AddMember("graphVariable", "CycleMovesetsRandom", allocator);
        numVal.AddMember("graphVariableType", "Float", allocator);
        newRandom.AddMember("Numeric value", numVal, allocator);

        conditionsArray.PushBack(newRandom, allocator);
    }

    // Everything user-related is below here

    std::optional<size_t> AnimationManager::FindModIndexByName(const std::string& name) {
        for (size_t i = 0; i < _allMods.size(); ++i) {
            if (_allMods[i].name == name) {
                return i;
            }
        }
        return std::nullopt;
    }

    std::optional<size_t> AnimationManager::FindSubAnimIndexByName(size_t modIdx, const std::string& name) {
        if (modIdx >= _allMods.size()) return std::nullopt;
        const auto& modDef = _allMods[modIdx];
        for (size_t i = 0; i < modDef.subAnimations.size(); ++i) {
            if (modDef.subAnimations[i].name == name) {
                return i;
            }
        }
        return std::nullopt;
    }

    void AnimationManager::UpdateMaxMovesetCache() {
        SKSE::log::info("Updating maximum moveset count cache...");
        _maxMovesetsPerCategory.clear();
        _maxMovesetsPerCategory_NPC.clear();

        // 1. PLAYER cache (unchanged logic)
        for (auto& pair : _categories) {
            WeaponCategory& category = pair.second;
            std::array<int, 4> counts = {0, 0, 0, 0};
            for (int i = 0; i < 4; ++i) {
                CategoryInstance& instance = category.instances[i];
                int parentMovesetCount = 0;
                for (auto& modInst : instance.modInstances) {
                    if (!modInst.isSelected) continue;
                    for (auto& subInst : modInst.subAnimationInstances) {
                        if (!subInst.isSelected) continue;
                        const auto& sourceSubAnim =
                            _allMods[subInst.sourceModIndex].subAnimations[subInst.sourceSubAnimIndex];
                        if (!sourceSubAnim.hasAnimations) {
                            continue;
                        }
                        bool isParent = !(subInst.pFront || subInst.pBack || subInst.pLeft || subInst.pRight ||
                                          subInst.pFrontRight || subInst.pFrontLeft || subInst.pBackRight ||
                                          subInst.pBackLeft || subInst.pRandom || subInst.pDodge);
                        if (isParent) {
                            parentMovesetCount++;
                        }
                    }
                }
                counts[i] = parentMovesetCount;
            }
            _maxMovesetsPerCategory[category.name] = counts;
        }
        SKSE::log::info("Player cache updated.");

        // 2. GENERAL NPCs cache (using FormID 0 as key)
        for (auto& pair : _npcCategories) {
            WeaponCategory& category = pair.second;
            CategoryInstance& instance = category.instances[0];  // Stance 0 for NPCs
            int npcMovesetCount = 0;
            for (auto& modInst : instance.modInstances) {
                if (modInst.isSelected) {
                    for (auto& subInst : modInst.subAnimationInstances) {
                        if (subInst.isSelected) {
                            npcMovesetCount++;
                        }
                    }
                }
            }
            std::array<int, 4> npc_counts = {npcMovesetCount, 0, 0, 0};
            _maxMovesetsPerCategory_NPC[0][category.name] = npc_counts;
        }
        SKSE::log::info("General NPCs cache (ID 0) updated.");

        // 3. SPECIFIC NPCs cache (using their actual FormID as key)
        for (const auto& npcConfigPair : _specificNpcConfigs) {
            RE::FormID npcFormID = npcConfigPair.first;
            const SpecificNpcConfig& npcConfig = npcConfigPair.second;

            for (const auto& catPair : npcConfig.categories) {
                const WeaponCategory& category = catPair.second;
                const CategoryInstance& instance = category.instances[0];
                int npcMovesetCount = 0;
                for (const auto& modInst : instance.modInstances) {
                    if (modInst.isSelected) {
                        for (const auto& subInst : modInst.subAnimationInstances) {
                            if (subInst.isSelected) {
                                npcMovesetCount++;
                            }
                        }
                    }
                }
                std::array<int, 4> npc_counts = {npcMovesetCount, 0, 0, 0};
                _maxMovesetsPerCategory_NPC[npcFormID][category.name] = npc_counts;
            }
            SKSE::log::info("Cache for Specific NPC {:08X} updated.", npcFormID);
        }

        SKSE::log::info("Maximum moveset count cache (Player & All NPCs) has been updated.");
    }


    void AnimationManager::AddNegatedCompareValuesCondition(rapidjson::Value& conditionsArray,
                                                            const std::string& graphVarName, int value,
                                                            rapidjson::Document::AllocatorType& allocator) {
        rapidjson::Value newCompare(rapidjson::kObjectType);
        newCompare.AddMember("condition", "CompareValues", allocator);

        newCompare.AddMember("negated", true, allocator);

        newCompare.AddMember("requiredVersion", "1.0.0.0", allocator);
        rapidjson::Value valueA(rapidjson::kObjectType);
        valueA.AddMember("value", value, allocator);
        newCompare.AddMember("Value A", valueA, allocator);
        newCompare.AddMember("Comparison", "==", allocator);
        rapidjson::Value valueB(rapidjson::kObjectType);
        valueB.AddMember("graphVariable", rapidjson::Value(graphVarName.c_str(), allocator), allocator);
        valueB.AddMember("graphVariableType", "Int", allocator);
        newCompare.AddMember("Value B", valueB, allocator);
        conditionsArray.PushBack(newCompare, allocator);
    }

    // Helper function to create and add the complex condition block
    //void AnimationManager::AddOcfWeaponExclusionConditions(rapidjson::Value& parentArray,
    //                                                       rapidjson::Document::AllocatorType& allocator) {
    //    rapidjson::Value mainAndBlock(rapidjson::kObjectType);
    //    mainAndBlock.AddMember("condition", "AND", allocator);
    //    mainAndBlock.AddMember("requiredVersion", "1.0.0.0", allocator);

    //    rapidjson::Value innerConditions(rapidjson::kArrayType);

    //    // List of weapon editorIDs to be excluded
    //    const std::vector<const char*> keywords = {
    //        "OCF_WeapTypeRapier1H", "OCF_WeapTypeRapier2H",    "OCF_WeapTypeKatana1H",   "OCF_WeapTypeKatana2H",
    //        "OCF_WeapTypePike1H",   "OCF_WeapTypePike2H",      "OCF_WeapTypeHalberd2H",  "OCF_WeapTypeHalberd1H",
    //        "OCF_WeapTypeClaw1H",   "OCF_WeapTypeTwinblade1H", "OCF_WeapTypeTwinblade2H"};

    //    // Lambda function to create a negated "IsEquippedHasKeyword" condition
    //    auto createNegatedKeywordCondition = [&](const char* editorID, bool isLeftHand) {
    //        rapidjson::Value condition(rapidjson::kObjectType);
    //        condition.AddMember("condition", "IsEquippedHasKeyword", allocator);
    //        condition.AddMember("requiredVersion", "1.0.0.0", allocator);
    //        condition.AddMember("negated", true, allocator);

    //        rapidjson::Value keywordObj(rapidjson::kObjectType);
    //        keywordObj.AddMember("editorID", rapidjson::StringRef(editorID), allocator);
    //        condition.AddMember("Keyword", keywordObj, allocator);

    //        condition.AddMember("Left hand", isLeftHand, allocator);
    //        return condition;
    //    };

    //    // Add the conditions for most weapons (right and left hand)
    //    for (const auto& keyword : keywords) {
    //        innerConditions.PushBack(createNegatedKeywordCondition(keyword, false), allocator);  // Left hand: false
    //        innerConditions.PushBack(createNegatedKeywordCondition(keyword, true), allocator);   // Left hand: true
    //    }

    //    // Special Quarterstaff cases that don't follow the pair pattern
    //    innerConditions.PushBack(createNegatedKeywordCondition("OCF_WeapTypeQuarterstaff2H", false), allocator);
    //    innerConditions.PushBack(createNegatedKeywordCondition("OCF_WeapTypeQuarterstaff1H", true), allocator);

    //    mainAndBlock.AddMember("Conditions", innerConditions, allocator);
    //    parentArray.PushBack(mainAndBlock, allocator);
    //}

    void AnimationManager::AddCompetingKeywordExclusions(rapidjson::Value& parentArray,
                                                         const WeaponCategory* currentCategory, bool isLeftHand,
                                                         rapidjson::Document::AllocatorType& allocator) {
        // 1. Collect all competing keywords first
        std::vector<std::string> competingKeywords;
        for (const auto& pair : _categories) {
            const WeaponCategory& otherCategory = pair.second;
            if (otherCategory.name != currentCategory->name &&
                otherCategory.equippedTypeValue == currentCategory->equippedTypeValue && !otherCategory.keywords.empty()) {
                // Add all keywords from the competing category to the exclusion list
                competingKeywords.insert(competingKeywords.end(), otherCategory.keywords.begin(),
                                         otherCategory.keywords.end());
            }
        }

        if (competingKeywords.empty()) {
            return;
        }

        rapidjson::Value exclusionAndBlock(rapidjson::kObjectType);
        exclusionAndBlock.AddMember("condition", "AND", allocator);
        exclusionAndBlock.AddMember("comment", "Exclude competing weapon keywords", allocator);
        rapidjson::Value innerExclusionConditions(rapidjson::kArrayType);

        for (const auto& keyword : competingKeywords) {
            // The internal logic remains the same, since AddKeywordCondition already handles one keyword at a time
            AddKeywordCondition(innerExclusionConditions, keyword, isLeftHand, true, allocator);
        }

        exclusionAndBlock.AddMember("Conditions", innerExclusionConditions, allocator);
        parentArray.PushBack(exclusionAndBlock, allocator);
    }

    RuleType RuleTypeFromString(const std::string& s) {
        if (s == "UniqueNPC") return RuleType::UniqueNPC;
        if (s == "Keyword") return RuleType::Keyword;
        if (s == "Faction") return RuleType::Faction;
        if (s == "Race") return RuleType::Race;
        // Add other types if necessary
        return RuleType::GeneralNPC;  // A safe default
    }

    std::string RuleTypeToString(RuleType type) {
        switch (type) {
            case RuleType::UniqueNPC:
                return "UniqueNPC";
            case RuleType::Keyword:
                return "Keyword";
            case RuleType::Faction:
                return "Faction";
            case RuleType::Race:
                return "Race";
            case RuleType::GeneralNPC:
            default:
                return "GeneralNPC";
        }
    }

    void AnimationManager::SaveCycleMovesets() {
        SKSE::log::info("Starting to save the UI state to User_CycleMoveset.json files...");

        std::map<std::filesystem::path, std::unique_ptr<rapidjson::Document>> documents;
        std::set<std::filesystem::path> requiredFiles;

        auto processActorCategories = [&](const std::map<std::string, WeaponCategory>& sourceCategories,
                                          const MovesetRule* rule = nullptr) {
            std::string actorTypeStr, actorName, actorFormIDStr, actorPlugin, actorIdentifier;
            if (rule) {
                actorTypeStr = RuleTypeToString(rule->type);
                actorName = rule->displayName;
                actorFormIDStr = std::format("{:08X}", rule->formID);
                actorPlugin = rule->pluginName;
                actorIdentifier = rule->identifier;
            } else {  // Player
                actorTypeStr = "Player";
                actorName = "Player";
                actorFormIDStr = "00000007";
                actorPlugin = "Skyrim.esm";
                actorIdentifier = "Player";
            }

            for (const auto& categoryPair : sourceCategories) {
                const WeaponCategory& category = categoryPair.second;
                const bool isPlayerRule = (rule == nullptr || rule->type == RuleType::Player);
                int stanceLimit = isPlayerRule ? 4 : 1;

                for (int i = 0; i < stanceLimit; ++i) {  // Stances
                    const CategoryInstance& instance = category.instances[i];
                    for (size_t mod_idx = 0; mod_idx < instance.modInstances.size(); ++mod_idx) {
                        const auto& modInst = instance.modInstances[mod_idx];
                        if (!modInst.isSelected) continue;

                        const auto& sourceMod = _allMods[modInst.sourceModIndex];

                        int animationIndexCounter = 1;
                        for (const auto& subInst : modInst.subAnimationInstances) {
                            if (!subInst.isSelected) continue;

                            const auto& animOriginMod = _allMods[subInst.sourceModIndex];
                            const auto& animOriginSub = animOriginMod.subAnimations[subInst.sourceSubAnimIndex];
                            std::filesystem::path destJsonPath;
                            // If the animation is from the DAR virtual mod, the path is the directory itself
                            if (animOriginMod.name == "[DAR] Animations") {
                                destJsonPath = animOriginSub.path / "User_CycleMoveset.json";
                            } else {  // Otherwise, it's the parent of config.json
                                destJsonPath = animOriginSub.path.parent_path() / "User_CycleMoveset.json";
                            }
                            requiredFiles.insert(destJsonPath);

                            if (documents.find(destJsonPath) == documents.end()) {
                                documents[destJsonPath] = std::make_unique<rapidjson::Document>();
                                documents[destJsonPath]->SetArray();
                            }
                            rapidjson::Document& doc = *documents[destJsonPath];
                            auto& allocator = doc.GetAllocator();

                            // 1. Find/Create the Actor Profile
                            rapidjson::Value* profileObj = nullptr;
                            for (auto& item : doc.GetArray()) {
                                if (item.IsObject() && item.HasMember("FormID") &&
                                    item["FormID"].GetString() == actorFormIDStr) {
                                    profileObj = &item;
                                    break;
                                }
                            }
                            if (!profileObj) {
                                rapidjson::Value newProfileObj(rapidjson::kObjectType);
                                newProfileObj.AddMember("Type", rapidjson::Value(actorTypeStr.c_str(), allocator),
                                                        allocator);
                                newProfileObj.AddMember("Name", rapidjson::Value(actorName.c_str(), allocator),
                                                        allocator);
                                newProfileObj.AddMember("FormID", rapidjson::Value(actorFormIDStr.c_str(), allocator),
                                                        allocator);
                                newProfileObj.AddMember("Plugin", rapidjson::Value(actorPlugin.c_str(), allocator),
                                                        allocator);
                                newProfileObj.AddMember(
                                    "Identifier", rapidjson::Value(actorIdentifier.c_str(), allocator), allocator);
                                newProfileObj.AddMember("Menu", rapidjson::kArrayType, allocator);
                                doc.PushBack(newProfileObj, allocator);
                                profileObj = &doc.GetArray()[doc.GetArray().Size() - 1];
                            }

                            // 2. Find/Create the Category
                            rapidjson::Value& menuArray = (*profileObj)["Menu"];
                            rapidjson::Value* categoryObj = nullptr;
                            for (auto& item : menuArray.GetArray()) {
                                if (item.IsObject() && item.HasMember("Category") &&
                                    item["Category"].GetString() == category.name) {
                                    categoryObj = &item;
                                    break;
                                }
                            }
                            if (!categoryObj) {
                                rapidjson::Value newCategoryObj(rapidjson::kObjectType);
                                newCategoryObj.AddMember("Category", rapidjson::Value(category.name.c_str(), allocator),
                                                         allocator);
                                newCategoryObj.AddMember("stances", rapidjson::kArrayType, allocator);
                                menuArray.PushBack(newCategoryObj, allocator);
                                categoryObj = &menuArray.GetArray()[menuArray.GetArray().Size() - 1];
                            }

                            // 3. Find/Create the Stance (the moveset)
                            rapidjson::Value& stancesArray = (*categoryObj)["stances"];
                            rapidjson::Value* stanceObj = nullptr;
                            for (auto& item : stancesArray.GetArray()) {
                                if (item.IsObject() && item["index"].GetInt() == (i + 1) && item.HasMember("name") &&
                                    strcmp(item["name"].GetString(), sourceMod.name.c_str()) == 0) {
                                    stanceObj = &item;
                                    break;
                                }
                            }
                            if (!stanceObj) {
                                rapidjson::Value newStanceObj(rapidjson::kObjectType);
                                newStanceObj.AddMember("index", i + 1, allocator);
                                newStanceObj.AddMember("type", "moveset", allocator);
                                newStanceObj.AddMember("name", rapidjson::Value(sourceMod.name.c_str(), allocator),
                                                       allocator);
                                newStanceObj.AddMember("level", modInst.level, allocator);
                                newStanceObj.AddMember("hp", modInst.hp, allocator);
                                newStanceObj.AddMember("st", modInst.st, allocator);
                                newStanceObj.AddMember("mn", modInst.mn, allocator);

                                // <<< MAIN CHANGE: Uses the loop index (mod_idx) to set the order
                                // We add +1 because the order in the JSON must start at 1, not 0.
                                newStanceObj.AddMember("order", static_cast<int>(mod_idx + 1), allocator);

                                newStanceObj.AddMember("animations", rapidjson::kArrayType, allocator);
                                stancesArray.PushBack(newStanceObj, allocator);
                                stanceObj = &stancesArray.GetArray()[stancesArray.GetArray().Size() - 1];
                            }

                            // 4. Add the individual Animation to the Stance's "animations" array
                            rapidjson::Value& animationsArray = (*stanceObj)["animations"];
                            rapidjson::Value animObj(rapidjson::kObjectType);
                            animObj.AddMember("index", animationIndexCounter++, allocator);
                            animObj.AddMember("sourceModName", rapidjson::Value(animOriginMod.name.c_str(), allocator),
                                              allocator);
                            const char* nameToSave = (subInst.editedName[0] != '\0') ? subInst.editedName.data()
                                                                                     : animOriginSub.name.c_str();


                            animObj.AddMember("sourceSubName", rapidjson::Value(nameToSave, allocator), allocator);
                            animObj.AddMember("hasDPA_A", animOriginSub.dpaTags.hasA, allocator);
                            animObj.AddMember("hasDPA_B", animOriginSub.dpaTags.hasB, allocator);
                            animObj.AddMember("hasDPA_L", animOriginSub.dpaTags.hasL, allocator);
                            animObj.AddMember("hasDPA_R", animOriginSub.dpaTags.hasR, allocator);
                            animObj.AddMember("hasCPA", animOriginSub.hasCPA, allocator);
                            animObj.AddMember("sourceConfigPath",
                                              rapidjson::Value(animOriginSub.path.string().c_str(), allocator),
                                              allocator);
                            animObj.AddMember("pFront", subInst.pFront, allocator);
                            animObj.AddMember("pBack", subInst.pBack, allocator);
                            animObj.AddMember("pLeft", subInst.pLeft, allocator);
                            animObj.AddMember("pRight", subInst.pRight, allocator);
                            animObj.AddMember("pFrontRight", subInst.pFrontRight, allocator);
                            animObj.AddMember("pFrontLeft", subInst.pFrontLeft, allocator);
                            animObj.AddMember("pBackRight", subInst.pBackRight, allocator);
                            animObj.AddMember("pBackLeft", subInst.pBackLeft, allocator);
                            animObj.AddMember("pRandom", subInst.pRandom, allocator);
                            animObj.AddMember("pDodge", subInst.pDodge, allocator);
                            animationsArray.PushBack(animObj, allocator);
                        }
                    }
                }
            }
        };

        // Process the Player
        processActorCategories(_categories, nullptr);
        // Process the General Rule
        processActorCategories(_generalNpcRule.categories, &_generalNpcRule);
        // Process the Specific Rules
        for (const auto& rule : _npcRules) {
            processActorCategories(rule.categories, &rule);
        }

        // Write the files to disk
        SKSE::log::info("Writing {} User_CycleMoveset.json files...", documents.size());
        for (const auto& pair : documents) {
            const auto& path = pair.first;
            const auto& doc = pair.second;
            FILE* fp;
            fopen_s(&fp, path.string().c_str(), "wb");
            if (fp) {
                char writeBuffer[65536];
                rapidjson::FileWriteStream os(fp, writeBuffer, sizeof(writeBuffer));
                rapidjson::PrettyWriter<rapidjson::FileWriteStream> writer(os);
                doc->Accept(writer);
                fclose(fp);
            } else {
                SKSE::log::error("Failed to open for writing: {}", path.string());
            }
        }

        // Clean up orphaned files

        for (const auto& managedConfigPath : _managedFiles) {
            // Derive the UI file name from the managed config.json path
            std::filesystem::path userCycleMovesetPath = managedConfigPath.parent_path() / "User_CycleMoveset.json";

            // If the UI file wasn't required in this save operation, it's an orphan.
            if (requiredFiles.find(userCycleMovesetPath) == requiredFiles.end()) {
                // Whether cleaning up an existing file or creating a new one to override a fallback,
                // the operation is the same: write "[]" to the file.
                SKSE::log::info("Cleaning/Creating orphaned User_CycleMoveset.json at: {}", userCycleMovesetPath.string());

                std::ofstream ofs(userCycleMovesetPath, std::ofstream::trunc);
                if (ofs) {
                    ofs << "[]";  // Write an empty JSON array
                    ofs.close();
                } else {
                    SKSE::log::error("Failed to open to clean/create the file: {}", userCycleMovesetPath.string());
                }
            }
        }
        SKSE::log::info("Saving of {} User_CycleMoveset.json files complete.", documents.size());
    }


    void AnimationManager::LoadCycleMovesets() {
        SKSE::log::info("Starting to load rules from (User_)CycleMoveset.json files...");

        // Clear the current state to ensure a clean load
        for (auto& pair : _categories) {
            for (auto& instance : pair.second.instances) instance.modInstances.clear();
        }
        _generalNpcRule.categories = _categories;
        for (auto& pair : _generalNpcRule.categories) {
            for (auto& instance : pair.second.instances) instance.modInstances.clear();
        }
        _npcRules.clear();

        const std::filesystem::path oarRootPath = "Data\\meshes\\actors\\character\\animations\\OpenAnimationReplacer";
        if (!std::filesystem::exists(oarRootPath)) {
            SKSE::log::warn("OAR directory not found. Rule loading aborted.");
            return;
        }
        const std::filesystem::path darRootPath =
            "Data\\meshes\\actors\\character\\animations\\DynamicAnimationReplacer\\_CustomConditions";
       
        std::set<std::filesystem::path> processedFolders;

        auto processJsonFile = [&](const std::filesystem::path& jsonPath) {
            std::ifstream ifs(jsonPath);
            if (!ifs.is_open()) return;
            std::string jsonContent((std::istreambuf_iterator<char>(ifs)), {});
            ifs.close();
            rapidjson::Document doc;
            doc.Parse(jsonContent.c_str());

            if (doc.HasParseError() || !doc.IsArray()) {
                SKSE::log::warn("Malformed file or not an array, skipping: {}", jsonPath.string());
                return;
            }

            for (const auto& profile : doc.GetArray()) {
                if (!profile.IsObject() || !profile.HasMember("Type") || !profile.HasMember("Menu") ||
                    !profile.HasMember("FormID"))
                    continue;

                std::string type = profile["Type"].GetString();
                std::string formIdStr = profile["FormID"].GetString();
                const rapidjson::Value& menu = profile["Menu"];
                if (!menu.IsArray()) continue;

                std::map<std::string, WeaponCategory>* targetCategories = nullptr;

                if (type == "Player") {
                    targetCategories = &_categories;
                } else if (type == "GeneralNPC") {
                    targetCategories = &_generalNpcRule.categories;
                    _generalNpcRule.displayName = "NPCs (General)";
                    _generalNpcRule.type = RuleType::GeneralNPC;
                    _generalNpcRule.formID = 0xFFFFFFFF;  // Sentinel ID
                } else {
                    // FIXED SEARCH LOGIC
                    auto rule_it = std::find_if(_npcRules.begin(), _npcRules.end(), [&](const MovesetRule& r) {
                        return std::format("{:08X}", r.formID) == formIdStr;
                    });

                    if (rule_it == _npcRules.end()) {
                        MovesetRule newRule;
                        newRule.type = RuleTypeFromString(type);
                        newRule.displayName = profile["Name"].GetString();
                        newRule.identifier = profile["Identifier"].GetString();
                        newRule.pluginName = profile["Plugin"].GetString();

                        try {
                            newRule.formID = std::stoul(formIdStr, nullptr, 16);
                        } catch (const std::exception&) {
                            continue;
                        }

                        newRule.categories = _categories;  // Start with a clean copy
                        for (auto& pair : newRule.categories) {
                            for (auto& instance : pair.second.instances) instance.modInstances.clear();
                        }
                        _npcRules.push_back(newRule);
                        targetCategories = &_npcRules.back().categories;
                    } else {
                        targetCategories = &rule_it->categories;
                    }
                }

                if (!targetCategories) continue;

                // Logic to populate the categories, stances, and animations
                for (const auto& categoryJson : menu.GetArray()) {
                    if (!categoryJson.IsObject() || !categoryJson.HasMember("Category") ||
                        !categoryJson.HasMember("stances"))
                        continue;
                    std::string categoryName = categoryJson["Category"].GetString();
                    auto categoryIt = targetCategories->find(categoryName);
                    if (categoryIt == targetCategories->end()) continue;

                    for (const auto& stanceJson : categoryJson["stances"].GetArray()) {
                        if (!stanceJson.IsObject() || !stanceJson.HasMember("index") || !stanceJson.HasMember("name") ||
                            !stanceJson.HasMember("animations"))
                            continue;

                        int stanceIndex = stanceJson["index"].GetInt();
                        if (stanceIndex < 1 || stanceIndex > 4) continue;

                        CategoryInstance& targetInstance = categoryIt->second.instances[stanceIndex - 1];
                        std::string movesetName = stanceJson["name"].GetString();
                        auto modIdxOpt = FindModIndexByName(movesetName);
                        if (!modIdxOpt) continue;

                        // --- RESTORED GROUPING LOGIC ---
                        ModInstance* modInstancePtr = nullptr;
                        int hp = stanceJson.HasMember("hp") ? stanceJson["hp"].GetInt() : 100;
                        int st = stanceJson.HasMember("st") ? stanceJson["st"].GetInt() : 100;
                        int mn = stanceJson.HasMember("mn") ? stanceJson["mn"].GetInt() : 100;
                        int level = stanceJson.HasMember("level") ? stanceJson["level"].GetInt() : 0;
                        int order = stanceJson.HasMember("order") ? stanceJson["order"].GetInt() : 0;

                        for (auto& mi : targetInstance.modInstances) {
                            // Now checks the name AND all conditions
                            if (mi.sourceModIndex == *modIdxOpt && mi.hp == hp && mi.st == st && mi.mn == mn &&
                                mi.level == level) {
                                modInstancePtr = &mi;
                                break;
                            }
                        }

                        if (!modInstancePtr) {
                            targetInstance.modInstances.emplace_back();
                            modInstancePtr = &targetInstance.modInstances.back();
                            modInstancePtr->sourceModIndex = *modIdxOpt;
                            modInstancePtr->isSelected = true;

                            // Apply the conditions when creating the new moveset
                            modInstancePtr->hp = hp;
                            modInstancePtr->st = st;
                            modInstancePtr->mn = mn;
                            modInstancePtr->level = level;
                            modInstancePtr->order = order;  // Uses the 'order' read from the JSON
                        }
                        // --- END OF GROUPING LOGIC ---

                        for (const auto& animJson : stanceJson["animations"].GetArray()) {
                            // Validation of essential fields. Now, sourceConfigPath is the most important.
                            if (!animJson.IsObject() || !animJson.HasMember("sourceConfigPath") ||
                                !animJson.HasMember("index"))
                                continue;

                            // --- IMPROVED SEARCH LOGIC ---

                            std::string configPathStr = animJson["sourceConfigPath"].GetString();
                            if (configPathStr.empty()) {
                                SKSE::log::warn("Found animation entry with empty sourceConfigPath. Skipping.");
                                continue;
                            }

                            // Look up the animation using the path as a unique ID
                            auto indicesOpt = FindSubAnimationByPath(configPathStr);

                            if (!indicesOpt) {
                                indicesOpt = FindSubAnimationByPath(configPathStr);
                                if (!indicesOpt) {
                                    SKSE::log::warn(
                                        "Could not find the animation for the config/path: {}. It may have been "
                                        "removed. Skipping.",
                                        configPathStr);
                                    continue;
                                }
                            }

                            SubAnimationInstance newSubInstance;
                            newSubInstance.sourceModIndex = indicesOpt->first;       // Mod Index
                            newSubInstance.sourceSubAnimIndex = indicesOpt->second;  // Sub-Animation Index
                            if (animJson.HasMember("sourceSubName") && animJson["sourceSubName"].IsString()) {
                                const char* savedName = animJson["sourceSubName"].GetString();
                                const auto& originSubAnim = _allMods[newSubInstance.sourceModIndex]
                                                                .subAnimations[newSubInstance.sourceSubAnimIndex];
                                if (strcmp(savedName, originSubAnim.name.c_str()) != 0) {
                                    // Copy the saved name to the new instance's buffer.
                                    strcpy_s(newSubInstance.editedName.data(), newSubInstance.editedName.size(),
                                             savedName);
                                }
                            }
                            if (animJson.HasMember("hasDPA_A") && animJson["hasDPA_A"].IsBool())
                                newSubInstance.dpaTags.hasA = animJson["hasDPA_A"].GetBool();
                            if (animJson.HasMember("hasDPA_B") && animJson["hasDPA_B"].IsBool())
                                newSubInstance.dpaTags.hasB = animJson["hasDPA_B"].GetBool();
                            if (animJson.HasMember("hasDPA_L") && animJson["hasDPA_L"].IsBool())
                                newSubInstance.dpaTags.hasL = animJson["hasDPA_L"].GetBool();
                            if (animJson.HasMember("hasDPA_R") && animJson["hasDPA_R"].IsBool())
                                newSubInstance.dpaTags.hasR = animJson["hasDPA_R"].GetBool();
                            if (animJson.HasMember("hasCPA") && animJson["hasCPA"].IsBool()) {
                                newSubInstance.hasCPA = animJson["hasCPA"].GetBool();
                            }

                            // --- END OF IMPROVED SEARCH LOGIC ---

                            // (The logic to populate the pFront, pBack, etc. flags remains the same)
                            if (animJson.HasMember("pFront")) newSubInstance.pFront = animJson["pFront"].GetBool();
                            if (animJson.HasMember("pBack")) newSubInstance.pBack = animJson["pBack"].GetBool();
                            if (animJson.HasMember("pLeft")) newSubInstance.pLeft = animJson["pLeft"].GetBool();
                            if (animJson.HasMember("pRight")) newSubInstance.pRight = animJson["pRight"].GetBool();
                            if (animJson.HasMember("pFrontRight"))
                                newSubInstance.pFrontRight = animJson["pFrontRight"].GetBool();
                            if (animJson.HasMember("pFrontLeft"))
                                newSubInstance.pFrontLeft = animJson["pFrontLeft"].GetBool();
                            if (animJson.HasMember("pBackRight"))
                                newSubInstance.pBackRight = animJson["pBackRight"].GetBool();
                            if (animJson.HasMember("pBackLeft"))
                                newSubInstance.pBackLeft = animJson["pBackLeft"].GetBool();
                            if (animJson.HasMember("pRandom")) newSubInstance.pRandom = animJson["pRandom"].GetBool();
                            if (animJson.HasMember("pDodge")) newSubInstance.pDodge = animJson["pDodge"].GetBool();

                            newSubInstance.isSelected = true;  // If it's in the file, it was selected.

                            // (The logic to insert at the correct position via "index" remains the same)
                            int subAnimIndex = animJson["index"].GetInt();
                            if (subAnimIndex < 1) continue;
                            if (modInstancePtr->subAnimationInstances.size() < subAnimIndex) {
                                modInstancePtr->subAnimationInstances.resize(subAnimIndex);
                            }
                            modInstancePtr->subAnimationInstances[subAnimIndex - 1] = newSubInstance;
                        }
                    }
                }
            }
        };

        if (std::filesystem::exists(oarRootPath)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(oarRootPath)) {
                // We will only look for folders that contain a config.json, which define a sub-moveset.
                if (entry.is_regular_file() && entry.path().filename() == "config.json") {
                    std::filesystem::path currentFolder = entry.path().parent_path();

                    std::filesystem::path userFile = currentFolder / "User_CycleMoveset.json";
                    std::filesystem::path defaultFile = currentFolder / "CycleMoveset.json";

                    bool userFileExists = std::filesystem::exists(userFile);

                    // Scenario 1: User_CycleMoveset.json exists.
                    if (userFileExists) {
                        // We try to process it. The function returns `true` if it had content and was processed.
                        // If the file exists but is empty or malformed, the function returns `false`
                        // and we do NOT try to load the fallback file, respecting the user's intent.
                        processJsonFile(userFile);
                    }
                    // Scenario 2: User_CycleMoveset.json does NOT exist.
                    else {
                        // We look for the fallback file.
                        if (std::filesystem::exists(defaultFile)) {
                            processJsonFile(defaultFile);
                        }
                    }
                }
            }
        }
        if (std::filesystem::exists(darRootPath)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(darRootPath)) {
                // We will only look for folders that contain a config.json, which define a sub-moveset.
                if (entry.is_regular_file() && entry.path().filename() == "user.json") {
                    std::filesystem::path currentFolder = entry.path().parent_path();

                    std::filesystem::path userFile = currentFolder / "User_CycleMoveset.json";
                    std::filesystem::path defaultFile = currentFolder / "CycleMoveset.json";

                    bool userFileExists = std::filesystem::exists(userFile);

                    // Scenario 1: User_CycleMoveset.json exists.
                    if (userFileExists) {
                        // We try to process it. The function returns `true` if it had content and was processed.
                        // If the file exists but is empty or malformed, the function returns `false`
                        // and we do NOT try to load the fallback file, respecting the user's intent.
                        processJsonFile(userFile);
                    }
                    // Scenario 2: User_CycleMoveset.json does NOT exist.
                    else {
                        // We look for the fallback file.
                        if (std::filesystem::exists(defaultFile)) {
                            processJsonFile(defaultFile);
                        }
                    }
                }
            }
        }
        

        // <<< CHANGE: Adds a sorting step AFTER loading all files
        SKSE::log::info("Sorting movesets based on the defined priority...");

        // Helper function to sort the movesets within a collection of categories
        auto sortMovesets = [](std::map<std::string, WeaponCategory>& categories) {
            for (auto& categoryPair : categories) {
                for (auto& instance : categoryPair.second.instances) {
                    std::sort(instance.modInstances.begin(), instance.modInstances.end(),
                              [](const ModInstance& a, const ModInstance& b) {
                                  // Sort by the 'order' field in ascending order
                                  return a.order < b.order;
                              });
                }
            }
        };

        // Apply the sorting to all rules (Player, GeneralNPC, and specific NPCs)
        sortMovesets(_categories);
        sortMovesets(_generalNpcRule.categories);
        for (auto& rule : _npcRules) {
            sortMovesets(rule.categories);
        }

        SKSE::log::info("Rule loading complete.");
        UpdateMaxMovesetCache();
    }


    void AnimationManager::SaveNPCSettings() {
        SKSE::log::info("Starting to save NPC settings...");
        std::map<std::filesystem::path, std::vector<FileSaveConfig>> fileUpdates;

        for (auto& pair : _npcCategories) {
            WeaponCategory& category = pair.second;
            // NPCs only use stance 0
            CategoryInstance& instance = category.instances[0];
            int playlistParentCounter = 1;

            for (auto& modInstance : instance.modInstances) {
                if (!modInstance.isSelected) continue;

                for (auto& subInstance : modInstance.subAnimationInstances) {
                    if (!subInstance.isSelected) continue;

                    const auto& sourceMod = _allMods[subInstance.sourceModIndex];
                    const auto& sourceSubAnim = sourceMod.subAnimations[subInstance.sourceSubAnimIndex];

                    FileSaveConfig config;
                    config.isNPC = true;        // Sets the flag for NPC
                    config.instance_index = 0;  // Stance 0 for NPCs
                    config.category = &category;
                    config.isParent = true;  // NPCs don't have directionals, so everything is a "Parent"
                    config.order_in_playlist = playlistParentCounter++;

                    fileUpdates[sourceSubAnim.path].push_back(config);
                }
            }
        }

        SKSE::log::info("{} NPC configuration files will be modified.", fileUpdates.size());
        for (const auto& updateEntry : fileUpdates) {
            // The same UpdateOrCreateJson function is called!
            UpdateOrCreateJson(updateEntry.first, updateEntry.second);
        }
        RE::DebugNotification("NPC settings saved!");
    }

    void AnimationManager::AddKeywordCondition(rapidjson::Value& parentArray, const std::string& editorID, bool isLeftHand,
                                               bool negated, rapidjson::Document::AllocatorType& allocator) {
        if (editorID.empty()) return;  // Do nothing if the keyword is empty

        rapidjson::Value condition(rapidjson::kObjectType);
        condition.AddMember("condition", "IsEquippedHasKeyword", allocator);
        condition.AddMember("requiredVersion", "1.0.0.0", allocator);

        // Add the "negated" flag if necessary
        if (negated) {
            condition.AddMember("negated", true, allocator);
        }

        rapidjson::Value keywordObj(rapidjson::kObjectType);
        keywordObj.AddMember("editorID", rapidjson::Value(editorID.c_str(), allocator), allocator);
        condition.AddMember("Keyword", keywordObj, allocator);

        condition.AddMember("Left hand", isLeftHand, allocator);
        parentArray.PushBack(condition, allocator);
    }

    void AnimationManager::AddKeywordOrConditions(rapidjson::Value& parentArray, const std::vector<std::string>& keywords,
                                                  bool isLeftHand, rapidjson::Document::AllocatorType& allocator) {
        // If there are no keywords to check, do nothing.
        if (keywords.empty()) {
            return;
        }

        // If there is only ONE keyword, we don't need an OR block. We add the condition directly.
        if (keywords.size() == 1) {
            AddKeywordCondition(parentArray, keywords[0], isLeftHand, false, allocator);
            return;
        }

        // If there is more than one keyword, we create the OR block
        rapidjson::Value orBlock(rapidjson::kObjectType);
        orBlock.AddMember("condition", "OR", allocator);
        orBlock.AddMember("comment", "Matches any of the required keywords", allocator);

        rapidjson::Value innerOrConditions(rapidjson::kArrayType);

        // Add each keyword as a condition inside the OR block
        for (const auto& keyword : keywords) {
            AddKeywordCondition(innerOrConditions, keyword, isLeftHand, false, allocator);
        }

        orBlock.AddMember("Conditions", innerOrConditions, allocator);
        parentArray.PushBack(orBlock, allocator);
    }

    // Function to look up the stance name
    std::string AnimationManager::GetStanceName(const std::string& categoryName, int stanceIndex) {
        if (stanceIndex < 0 || stanceIndex >= 4) {
            return "Invalid Stance";
        }
        auto it = _categories.find(categoryName);
        if (it != _categories.end()) {
            return it->second.stanceNames[stanceIndex];
        }
        return std::to_string(stanceIndex + 1);  // Fallback
    }


    // NEW FUNCTION: Look up the DPA and CPA tags for the active moveset (based on GetCurrentMovesetName)
    MovesetTags AnimationManager::GetCurrentMovesetTags(const std::string& categoryName,
                                                                          int stanceIndex, int movesetIndex) {
        auto animManager = AnimationManager::GetSingleton();
        if (movesetIndex <= 0) {
            return {false, false};  // Return default if there is no active moveset
        }

        auto cat_it = animManager->GetCategories().find(categoryName);
        if (cat_it == animManager->GetCategories().end()) {
            return {false, false};
        }

        const WeaponCategory& category = cat_it->second;
        if (stanceIndex < 0 || stanceIndex >= 4) {
            return {false, false};
        }

        const CategoryInstance& instance = category.instances[stanceIndex];
        const SubAnimationInstance* targetMoveset = nullptr;
        int parentCounter = 0;

        // The logic to find the active moveset is the same as in the GetCurrentMovesetName function
        for (const auto& modInst : instance.modInstances) {
            if (!modInst.isSelected) continue;
            for (const auto& subInst : modInst.subAnimationInstances) {
                if (!subInst.isSelected) continue;

                const auto& sourceSubAnim = _allMods[subInst.sourceModIndex].subAnimations[subInst.sourceSubAnimIndex];
                if (!sourceSubAnim.hasAnimations) {
                    continue;
                }

                bool isParent = !(subInst.pFront || subInst.pBack || subInst.pLeft || subInst.pRight ||
                                  subInst.pFrontRight || subInst.pFrontLeft || subInst.pBackRight ||
                                  subInst.pBackLeft || subInst.pRandom || subInst.pDodge);

                if (isParent) {
                    parentCounter++;
                    if (parentCounter == movesetIndex) {
                        targetMoveset = &subInst;
                        goto found_target;  // We found the parent moveset, we can stop searching
                    }
                }
            }
        }

    found_target:
        if (targetMoveset) {
            // Return the tags of the found moveset
            return {targetMoveset->dpaTags, targetMoveset->hasCPA};
        }

        // If not found (invalid index), return the default
        return {{}, false};
    }

    // Function to look up the moveset name
    std::string AnimationManager::GetCurrentMovesetName(const std::string& categoryName, int stanceIndex,
                                                        int movesetIndex, int directionalState) {
        //SKSE::log::info("==========================================================");
        //SKSE::log::info("[GetCurrentMovesetName] Invoked with: Category='{}', Stance={}, MovesetIndex={}, Direction={}",categoryName, stanceIndex, movesetIndex, directionalState);
        if (movesetIndex <= 0) {
            return "None";
        }

        auto cat_it = _categories.find(categoryName);
        if (cat_it == _categories.end()) {
            return "Category not found";
        }

        WeaponCategory& category = cat_it->second;
        if (stanceIndex < 0 || stanceIndex >= 4) {
            return "Invalid stance";
        }

        CategoryInstance& instance = category.instances[stanceIndex];

        int parentCounter = 0;
        const SubAnimationInstance* targetParent = nullptr;

        for (auto& modInst : instance.modInstances) {
            if (!modInst.isSelected) continue;
            for (auto& subInst : modInst.subAnimationInstances) {
                if (!subInst.isSelected) continue;

                const auto& sourceSubAnim = _allMods[subInst.sourceModIndex].subAnimations[subInst.sourceSubAnimIndex];
                if (!sourceSubAnim.hasAnimations) {
                    continue;
                }

                bool isParent = !(subInst.pFront || subInst.pBack || subInst.pLeft || subInst.pRight ||
                                  subInst.pFrontRight || subInst.pFrontLeft || subInst.pBackRight ||
                                  subInst.pBackLeft || subInst.pRandom || subInst.pDodge);

                if (isParent) {
                    parentCounter++;
                    if (parentCounter == movesetIndex) {
                        targetParent = &subInst;
                        // If a direction isn't needed, we already found what we wanted.
                        if (directionalState == 0) {
                            goto found_target;
                        }
                        // Otherwise, we keep scanning for children.
                    } else if (targetParent != nullptr) {
                        // If we already passed our target "parent" and found the NEXT "parent",
                        // it means there was no valid directional child in between. We stop the search.
                        goto found_target;
                    }
                } else if (targetParent != nullptr && directionalState != 0) {
                    // If we are in this part, it means we found a "child" animation AND
                    // we already passed the "parent" we were looking for.
                    bool isDirectionalMatch =
                        (directionalState == 1 && subInst.pFront) || (directionalState == 2 && subInst.pFrontRight) ||
                        (directionalState == 3 && subInst.pRight) || (directionalState == 4 && subInst.pBackRight) ||
                        (directionalState == 5 && subInst.pBack) || (directionalState == 6 && subInst.pBackLeft) ||
                        (directionalState == 7 && subInst.pLeft) || (directionalState == 8 && subInst.pFrontLeft);

                    if (isDirectionalMatch) {
                        // We found the directional child that matches our parent! This is the final result.
                        const auto& sourceSubAnimChild =
                            _allMods[subInst.sourceModIndex].subAnimations[subInst.sourceSubAnimIndex];
                        return (subInst.editedName[0] != '\0') ? subInst.editedName.data() : sourceSubAnimChild.name;
                    }
                }
            }
        }

    found_target:
        // If we got here, either we found the parent and didn't need a direction, or we didn't find a valid child.
        // In both cases, we return the parent's name.
        if (targetParent) {
            const auto& sourceSubAnimParent =
                _allMods[targetParent->sourceModIndex].subAnimations[targetParent->sourceSubAnimIndex];
            return (targetParent->editedName[0] != '\0') ? targetParent->editedName.data() : sourceSubAnimParent.name;
        }

        // If the function ends here, the movesetIndex was invalid (e.g. asked for the 5th parent, but only 4 exist).
        //SKSE::log::warn("[GetCurrentMovesetName] No 'parent' moveset found for index {}", movesetIndex);
        return "Not found";
    }

    void AnimationManager::SaveStanceNames() {
        SKSE::log::info("Saving stance names to files separated by category...");
        const std::filesystem::path stancesFolderPath = "Data/SKSE/Plugins/CycleMovesets/Stances";

        try {
            // Ensure the "Stances" directory exists
            if (!std::filesystem::exists(stancesFolderPath)) {
                std::filesystem::create_directories(stancesFolderPath);
            }
        } catch (const std::filesystem::filesystem_error& e) {
            SKSE::log::error("Failed to create the stances directory: {}. Error: {}", stancesFolderPath.string(), e.what());
            return;
        }

        // Iterate over each weapon category
        for (const auto& pair : _categories) {
            const WeaponCategory& category = pair.second;
            std::filesystem::path categorySavePath = stancesFolderPath / (category.name + ".json");

            rapidjson::Document doc;
            doc.SetArray();  // The root document will be an array
            auto& allocator = doc.GetAllocator();

            // Add the 4 stance names to the array
            for (const auto& name : category.stanceNames) {
                doc.PushBack(rapidjson::Value(name.c_str(), allocator), allocator);
            }

            // Write the JSON file specific to this category
            std::ofstream ofs(categorySavePath);
            if (!ofs) {
                SKSE::log::error("Failed to open {} for writing!", categorySavePath.string());
                continue;  // Skip to the next category in case of error
            }

            rapidjson::StringBuffer buffer;
            rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
            doc.Accept(writer);
            ofs << buffer.GetString();
            ofs.close();
        }

        SKSE::log::info("Stance names successfully saved to individual files.");
    }

    void AnimationManager::LoadStanceNames() {
        SKSE::log::info("Loading stance names from individual per-category files...");
        const std::filesystem::path stancesFolderPath = "Data/SKSE/Plugins/CycleMovesets/Stances";

        if (!std::filesystem::exists(stancesFolderPath)) {
            SKSE::log::info("Stance names directory not found. Using defaults.");
            return;
        }

        // Iterate over each weapon category to load its respective file
        for (auto& pair : _categories) {
            WeaponCategory& category = pair.second;
            std::filesystem::path categoryLoadPath = stancesFolderPath / (category.name + ".json");

            if (!std::filesystem::exists(categoryLoadPath)) {
                // If the file for this category doesn't exist, just skip to the next one
                continue;
            }

            std::ifstream ifs(categoryLoadPath);
            if (!ifs) {
                SKSE::log::error("Failed to open {} for reading!", categoryLoadPath.string());
                continue;
            }

            std::string jsonContent((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            ifs.close();

            rapidjson::Document doc;
            doc.Parse(jsonContent.c_str());

            if (doc.HasParseError() || !doc.IsArray()) {
                SKSE::log::error("JSON parse error or the file is not an array for category: {}", category.name);
                continue;
            }

            const auto& stanceNamesArray = doc.GetArray();
            for (rapidjson::SizeType i = 0; i < stanceNamesArray.Size() && i < 4; ++i) {
                if (stanceNamesArray[i].IsString()) {
                    category.stanceNames[i] = stanceNamesArray[i].GetString();
                    // Also update the ImGui buffer so the UI reflects the change
                    strcpy_s(category.stanceNameBuffers[i].data(), category.stanceNameBuffers[i].size(),
                             category.stanceNames[i].c_str());
                }
            }
        }

        SKSE::log::info("Individual stance names loaded successfully.");
    }

    void AnimationManager::DrawStanceEditorPopup() {
        if (_isEditStanceModalOpen) {
            ImGui::OpenPopup(LOC("edit_stance_name_popup"));
            _isEditStanceModalOpen = false;  // Reset the trigger
        }

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 center = ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f);
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        if (ImGui::BeginPopupModal(LOC("edit_stance_name_popup"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text(LOC("enter_new_stance_name"));
            ImGui::Separator();

            ImGui::PushItemWidth(300);
            ImGui::InputText("##NewStanceName", _editStanceNameBuffer, sizeof(_editStanceNameBuffer));
            ImGui::PopItemWidth();

            if (ImGui::Button(LOC("save"), ImVec2(120, 0))) {
                if (_categoryToEdit && _stanceIndexToEdit != -1) {
                    // Save the name from the temporary buffer to the real data structure
                    _categoryToEdit->stanceNames[_stanceIndexToEdit] = _editStanceNameBuffer;
                    // And also to the buffer that the Tab uses, for instant visual update
                    strcpy_s(_categoryToEdit->stanceNameBuffers[_stanceIndexToEdit].data(),
                             _categoryToEdit->stanceNameBuffers[_stanceIndexToEdit].size(), _editStanceNameBuffer);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(LOC("cancel"), ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    void AnimationManager::DrawRestartPopup() {
        // If the flag is true, we tell ImGui to open the popup on the next frame
        if (_showRestartPopup) {
            ImGui::OpenPopup("Restart Required");
            _showRestartPopup = false;  // Reset the flag so it doesn't open every time
        }

        // Configure the popup's central position
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 center = ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f);
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        // Define the popup content
        if (ImGui::BeginPopupModal("Restart Required", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Configs saved, reload the game to take effect.");
            ImGui::Separator();

            // Center the OK button
            float window_width = ImGui::GetWindowWidth();
            float button_width = 120.0f;
            ImGui::SetCursorPosX((window_width - button_width) * 0.5f);

            if (ImGui::Button("OK", ImVec2(button_width, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    void AnimationManager::SaveUserMoveset() {
        std::string movesetName = _newMovesetName;
        if (movesetName.empty()) {
            RE::DebugNotification("ERROR: Moveset name cannot be empty!");
            return;
        }

        std::vector<WeaponCategory*> selectedCategories;
        for (const auto& [name, isSelected] : _newMovesetCategorySelection) {
            if (isSelected) {
                auto it = _categories.find(name);
                if (it != _categories.end()) {
                    selectedCategories.push_back(&it->second);
                }
            }
        }

        if (selectedCategories.empty()) {
            RE::DebugNotification("ERROR: At least one weapon category must be selected!");
            return;
        }

        SKSE::log::info("Starting to save user moveset: {}", movesetName);
        const std::filesystem::path oarRootPath = "Data\\meshes\\actors\\character\\animations\\OpenAnimationReplacer";
        std::filesystem::path newMovesetPath = oarRootPath / movesetName;

        try {
            std::filesystem::create_directories(newMovesetPath);

            rapidjson::Document doc;
            doc.SetObject();
            auto& allocator = doc.GetAllocator();
            doc.AddMember("name", rapidjson::Value(movesetName.c_str(), allocator), allocator);
            doc.AddMember("author", rapidjson::Value(_newMovesetAuthor, allocator), allocator);
            doc.AddMember("description", rapidjson::Value(_newMovesetDesc, allocator), allocator);

            std::ofstream outFile(newMovesetPath / "config.json");
            rapidjson::StringBuffer buffer;
            rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
            doc.Accept(writer);
            outFile << buffer.GetString();
        } catch (const std::filesystem::filesystem_error& e) {
            SKSE::log::error("Failed to create the moveset folder: {}. Error: {}", newMovesetPath.string(), e.what());
            RE::DebugNotification("ERROR: Failed to create moveset folder!");
            return;
        }

        // NEW STRUCTURE TO GROUP SAVE DATA
        struct SubmovesetSaveData {
            // Stores all instances that share the same edited name
            std::vector<const CreatorSubAnimationInstance*> instances;
            std::vector<FileSaveConfig> configs;
        };
        std::map<std::string, SubmovesetSaveData> uniqueSubmovesets;

        for (WeaponCategory* cat : selectedCategories) {
            if (_movesetCreatorStances.find(cat->name) == _movesetCreatorStances.end()) continue;

            auto& stances = _movesetCreatorStances.at(cat->name);
            for (int i = 0; i < 4; ++i) {
                // The _newMovesetStanceEnabled flag was removed since it didn't exist in the original code,
                // the check will be done via the existence of submovesets in the stance.
                if (stances[i].subMovesets.empty()) continue;

                int playlistParentCounter = 1;
                int lastParentOrder = 0;

                for (const auto& subInst : stances[i].subMovesets) {
                    std::string subName = subInst.editedName.data();
                    if (subName.empty()) continue;

                    bool isParent = !(subInst.pFront || subInst.pBack || subInst.pLeft || subInst.pRight ||
                                      subInst.pFrontRight || subInst.pFrontLeft || subInst.pBackRight ||
                                      subInst.pBackLeft || subInst.pRandom || subInst.pDodge);

                    int order = isParent ? playlistParentCounter++ : lastParentOrder;
                    if (isParent) lastParentOrder = order;

                    FileSaveConfig config;

                    // ===== START OF FIX =====
                    // Add the Player rule info to the config.
                    // Movesets created by this tool are always for the Player.
                    config.ruleType = RuleType::Player;
                    config.formID = 0x7;
                    config.pluginName = "Skyrim.esm";
                    config.ruleIdentifier = "Player";
                    // ===== END OF FIX =====

                    config.category = cat;
                    config.instance_index = i + 1;
                    config.isParent = isParent;
                    config.order_in_playlist = order;
                    config.pFront = subInst.pFront;
                    config.pBack = subInst.pBack;
                    config.pLeft = subInst.pLeft;
                    config.pRight = subInst.pRight;
                    config.pFrontRight = subInst.pFrontRight;
                    config.pFrontLeft = subInst.pFrontLeft;
                    config.pBackRight = subInst.pBackRight;
                    config.pBackLeft = subInst.pBackLeft;
                    config.pRandom = subInst.pRandom;
                    config.pDodge = subInst.pDodge;

                    uniqueSubmovesets[subName].configs.push_back(config);
                    uniqueSubmovesets[subName].instances.push_back(&subInst);
                }
            }
        }

        for (const auto& pair : uniqueSubmovesets) {
            const std::string& subName = pair.first;
            const SubmovesetSaveData& data = pair.second;
            std::filesystem::path subMovesetPath = newMovesetPath / subName;

            try {
                std::filesystem::create_directory(subMovesetPath);
            } catch (...) {
                continue;
            }

            UpdateOrCreateJson(subMovesetPath / "user.json", data.configs);

            // UPDATED LOGIC FOR CYCLEDAR.JSON
            {
                rapidjson::Document cycleDoc;
                cycleDoc.SetObject();
                auto& allocator = cycleDoc.GetAllocator();

                rapidjson::Value sourcesArray(rapidjson::kArrayType);
                bool anyBfco = false;

                // ===== START OF FIX =====
                // 1. Create a set to store the unique paths that have already been added to this JSON.
                std::set<std::string> uniquePaths;
                // ===== END OF FIX =====

                for (const auto* instancePtr : data.instances) {
                    if (!instancePtr || !instancePtr->sourceDef) continue;

                    std::string originalPathStr;
                    if (instancePtr->sourceDef->path.filename() == "config.json") {
                        originalPathStr = instancePtr->sourceDef->path.parent_path().string();
                    } else {
                        originalPathStr = instancePtr->sourceDef->path.string();
                    }
                    size_t pos = originalPathStr.find("Data\\");
                    if (pos != std::string::npos) {
                        originalPathStr = originalPathStr.substr(pos + 5);
                    }

                    // ===== START OF FIX =====
                    // 2. Try to insert the path into the set. The if block will only run if the path is new.
                    if (uniquePaths.insert(originalPathStr).second) {
                        // ===== END OF FIX =====

                        rapidjson::Value sourceObj(rapidjson::kObjectType);
                        sourceObj.AddMember("path", rapidjson::Value(originalPathStr.c_str(), allocator), allocator);

                        int selectedCount = 0;
                        rapidjson::Value filesArray(rapidjson::kArrayType);
                        for (const auto& [filename, isSelected] : instancePtr->hkxFileSelection) {
                            if (isSelected) {
                                filesArray.PushBack(rapidjson::Value(filename.c_str(), allocator), allocator);
                                selectedCount++;
                            }
                        }
                        if (selectedCount < instancePtr->hkxFileSelection.size() && selectedCount > 0) {
                            sourceObj.AddMember("filesToCopy", filesArray, allocator);
                        }
                        if (selectedCount == 0) continue;

                        sourcesArray.PushBack(sourceObj, allocator);
                        if (instancePtr->isBFCO) anyBfco = true;
                    }
                }

                cycleDoc.AddMember("sources", sourcesArray, allocator);
                cycleDoc.AddMember("conversionDone", false, allocator);
                cycleDoc.AddMember("convertBFCO", anyBfco, allocator);

                std::ofstream outFile(subMovesetPath / "CycleDar.json");
                rapidjson::StringBuffer buffer;
                rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
                cycleDoc.Accept(writer);
                outFile << buffer.GetString();
            }

            {
                rapidjson::Document cycleMovesetDoc;
                cycleMovesetDoc.SetArray();  // The root is an array
                auto& allocator = cycleMovesetDoc.GetAllocator();

                // 1. Create the Profile object for the Player
                rapidjson::Value profileObj(rapidjson::kObjectType);
                profileObj.AddMember("Type", "Player", allocator);
                profileObj.AddMember("Name", "Player", allocator);
                profileObj.AddMember("FormID", "00000007", allocator);
                profileObj.AddMember("Plugin", "Skyrim.esm", allocator);
                profileObj.AddMember("Identifier", "Player", allocator);

                rapidjson::Value menuArray(rapidjson::kArrayType);

                // 2. Group the settings by Category
                std::map<std::string, std::vector<const FileSaveConfig*>> configsByCategory;
                for (const auto& config : data.configs) {
                    configsByCategory[config.category->name].push_back(&config);
                }

                // 3. Iterate over each Category
                for (const auto& catPair : configsByCategory) {
                    rapidjson::Value categoryObj(rapidjson::kObjectType);
                    categoryObj.AddMember("Category", rapidjson::Value(catPair.first.c_str(), allocator), allocator);
                    rapidjson::Value stancesArray(rapidjson::kArrayType);

                    // 4. Group the settings by Stance
                    std::map<int, std::vector<const FileSaveConfig*>> configsByStance;
                    for (const auto* configPtr : catPair.second) {
                        configsByStance[configPtr->instance_index].push_back(configPtr);
                    }

                    // 5. Iterate over each Stance
                    for (const auto& stancePair : configsByStance) {
                        rapidjson::Value newStanceObj(rapidjson::kObjectType);
                        newStanceObj.AddMember("index", stancePair.first, allocator);
                        newStanceObj.AddMember("type", "moveset", allocator);
                        newStanceObj.AddMember("name", rapidjson::Value(movesetName.c_str(), allocator), allocator);
                        newStanceObj.AddMember("level", 0, allocator);
                        newStanceObj.AddMember("hp", 100, allocator);
                        newStanceObj.AddMember("st", 100, allocator);
                        newStanceObj.AddMember("mn", 100, allocator);
                        newStanceObj.AddMember("order", 1, allocator);

                        rapidjson::Value animationsArray(rapidjson::kArrayType);

                        // 6. Add each animation (sub-moveset) to the Stance
                        for (const auto* configPtr : stancePair.second) {
                            rapidjson::Value animObj(rapidjson::kObjectType);
                            animObj.AddMember("index", configPtr->order_in_playlist, allocator);
                            animObj.AddMember("sourceModName", rapidjson::Value(movesetName.c_str(), allocator),
                                              allocator);
                            animObj.AddMember("sourceSubName", rapidjson::Value(subName.c_str(), allocator), allocator);

                            // The path of the config.json that the Load function will look for
                            std::string configPathStr = (subMovesetPath / "config.json").string();
                            animObj.AddMember("sourceConfigPath", rapidjson::Value(configPathStr.c_str(), allocator),
                                              allocator);

                            animObj.AddMember("pFront", configPtr->pFront, allocator);
                            animObj.AddMember("pBack", configPtr->pBack, allocator);
                            animObj.AddMember("pLeft", configPtr->pLeft, allocator);
                            animObj.AddMember("pRight", configPtr->pRight, allocator);
                            animObj.AddMember("pFrontRight", configPtr->pFrontRight, allocator);
                            animObj.AddMember("pFrontLeft", configPtr->pFrontLeft, allocator);
                            animObj.AddMember("pBackRight", configPtr->pBackRight, allocator);
                            animObj.AddMember("pBackLeft", configPtr->pBackLeft, allocator);
                            animObj.AddMember("pRandom", configPtr->pRandom, allocator);
                            animObj.AddMember("pDodge", configPtr->pDodge, allocator);
                            animationsArray.PushBack(animObj, allocator);
                        }
                        newStanceObj.AddMember("animations", animationsArray, allocator);
                        stancesArray.PushBack(newStanceObj, allocator);
                    }
                    categoryObj.AddMember("stances", stancesArray, allocator);
                    menuArray.PushBack(categoryObj, allocator);
                }

                profileObj.AddMember("Menu", menuArray, allocator);
                cycleMovesetDoc.PushBack(profileObj, allocator);

                // 7. Save the final file
                std::ofstream outFile(subMovesetPath / "CycleMoveset.json");
                rapidjson::StringBuffer buffer;
                rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
                cycleMovesetDoc.Accept(writer);
                outFile << buffer.GetString();
            }
            
        }

        _newMovesetCategorySelection.clear();
        _movesetCreatorStances.clear();

        SKSE::log::info("Saving of moveset '{}' complete.", movesetName);
        RE::DebugNotification(std::format("Moveset '{}' saved successfully!", movesetName).c_str());
        _showRestartPopup = true;
    }

    void AnimationManager::ScanDarAnimations() {
        SKSE::log::info("[ScanDarAnimations] Starting the DAR scanning function.");
        try {
            _darSubMovesets.clear();
            SKSE::log::info("[ScanDarAnimations] _darSubMovesets vector was cleared.");

            const std::filesystem::path darRootPath =
                "Data\\meshes\\actors\\character\\animations\\DynamicAnimationReplacer\\_CustomConditions";

            // Converting to std::string for the log
            auto u8_darRootPath = darRootPath.u8string();
            SKSE::log::info("[ScanDarAnimations] Path to be checked: {}",
                            std::string(u8_darRootPath.begin(), u8_darRootPath.end()));

            if (!std::filesystem::exists(darRootPath) || !std::filesystem::is_directory(darRootPath)) {
                SKSE::log::warn("[ScanDarAnimations] The DAR root folder (_CustomConditions) was not found at '{}'.",
                                std::string(u8_darRootPath.begin(), u8_darRootPath.end()));
                RE::DebugNotification("DAR folder (_CustomConditions) not found.");
                return;
            }

            SKSE::log::info("[ScanDarAnimations] Folder found. Starting iteration over subfolders...");
            int folderCount = 0;
            for (const auto& entry : std::filesystem::directory_iterator(darRootPath)) {
                folderCount++;

                auto u8_entryPath = entry.path().u8string();
                SKSE::log::info("[ScanDarAnimations] [LOOP {}] Checking entry: '{}'", folderCount,
                                std::string(u8_entryPath.begin(), u8_entryPath.end()));

                if (entry.is_directory()) {
                    SKSE::log::info("[ScanDarAnimations] [LOOP {}] It's a directory. Processing...", folderCount);

                    SubAnimationDef subAnimDef;

                    SKSE::log::info("[ScanDarAnimations] [LOOP {}] Extracting folder name...", folderCount);

                    // ===================================================================
                    // MAIN FIX: Explicitly convert std::u8string to std::string
                    // ===================================================================
                    auto u8_filename = entry.path().filename().u8string();
                    subAnimDef.name = std::string(u8_filename.begin(), u8_filename.end());
                    SKSE::log::info("[ScanDarAnimations] [LOOP {}] Extracted name: '{}'", folderCount, subAnimDef.name);

                    subAnimDef.path = entry.path();
                    auto u8_subAnimPath = subAnimDef.path.u8string();
                    SKSE::log::info("[ScanDarAnimations] [LOOP {}] Path set to: '{}'", folderCount,
                                    std::string(u8_subAnimPath.begin(), u8_subAnimPath.end()));

                    SKSE::log::info("[ScanDarAnimations] [LOOP {}] Calling ScanSubAnimationFolderForTags for '{}'...",
                                    folderCount, subAnimDef.name);
                    ScanSubAnimationFolderForTags(entry.path(), subAnimDef);
                    SKSE::log::info(
                        "[ScanDarAnimations] [LOOP {}] Returned from ScanSubAnimationFolderForTags. The folder has animations: "
                        "{}",
                        folderCount, subAnimDef.hasAnimations);

                    if (subAnimDef.hasAnimations) {
                        SKSE::log::info("[ScanDarAnimations] [LOOP {}] The submoveset has animations. Adding to the vector...",
                                        folderCount);
                        _darSubMovesets.push_back(subAnimDef);
                        SKSE::log::info("[ScanDarAnimations] [LOOP {}] Successfully added: '{}'", folderCount,
                                        subAnimDef.name);
                    } else {
                        SKSE::log::info(
                            "[ScanDarAnimations] [LOOP {}] The submoveset '{}' does not contain .hkx files and will be skipped.",
                            folderCount, subAnimDef.name);
                    }
                } else {
                    SKSE::log::info("[ScanDarAnimations] [LOOP {}] The entry is not a directory. Skipping.", folderCount);
                }
            }
        } catch (const std::filesystem::filesystem_error& e) {
            SKSE::log::critical("[ScanDarAnimations] CRASH! FILESYSTEM ERROR DURING SCAN: {}", e.what());
            RE::DebugNotification("SEVERE ERROR reading DAR folders! Check the logs.");
        } catch (const std::exception& e) {
            SKSE::log::critical("[ScanDarAnimations] CRASH! GENERAL ERROR DURING SCAN: {}", e.what());
            RE::DebugNotification("SEVERE ERROR reading DAR folders! Check the logs.");
        } catch (...) {
            SKSE::log::critical("[ScanDarAnimations] CRASH! UNKNOWN AND UNIDENTIFIED ERROR DURING SCAN!");
            RE::DebugNotification("SEVERE AND UNKNOWN ERROR reading DAR folders! Check the logs.");
        }

        SKSE::log::info("[ScanDarAnimations] Scan finished. Total of {} submovesets loaded.",
                        _darSubMovesets.size());
        if (!_darSubMovesets.empty()) {
            RE::DebugNotification(std::format("{} DAR animations loaded.", _darSubMovesets.size()).c_str());
        }
    }

    void AnimationManager::DrawAddDarModal() {
        if (_isAddDarModalOpen) {
            ImGui::OpenPopup("Add DAR animation");
            _isAddDarModalOpen = false;
        }

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 modal_list_size = ImVec2(viewport->Size.x * 0.5f, viewport->Size.y * 0.5f);
        ImVec2 center = ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f);
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        if (ImGui::BeginPopupModal("Add DAR animation", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Library DAR");
            ImGui::Separator();
            static char darFilter[128] = "";
            ImGui::InputText(LOC("filter"), darFilter, sizeof(darFilter));
            ImGui::Separator();

            if (ImGui::BeginChild("DARLibrary", ImVec2(modal_list_size), true)) {
                std::string filter_str = darFilter;
                std::transform(filter_str.begin(), filter_str.end(), filter_str.begin(), ::tolower);

                for (size_t i = 0; i < _darSubMovesets.size(); ++i) {
                    const auto& darSubDef = _darSubMovesets[i];
                    std::string name_lower = darSubDef.name;
                    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);

                    if (filter_str.empty() || name_lower.find(filter_str) != std::string::npos) {
                        ImGui::PushID(static_cast<int>(i));
                        if (ImGui::Button(LOC("add"))) {
                            if (_stanceToAddTo) {
                                CreatorSubAnimationInstance newInstance;
                                // The pointer now points to an element in our _darSubMovesets vector
                                newInstance.sourceDef = &darSubDef;
                                strcpy_s(newInstance.editedName.data(), newInstance.editedName.size(),
                                         darSubDef.name.c_str());
                                PopulateHkxFiles(newInstance);
                                _stanceToAddTo->subMovesets.push_back(newInstance);
                                SKSE::log::info("Adding DAR animation '{}' to the stance.", darSubDef.name);
                            }
                        }
                        ImGui::SameLine();
                        ImGui::Text("%s", darSubDef.name.c_str());
                        ImGui::PopID();
                    }
                }
            }
            ImGui::EndChild();
            ImGui::Separator();
            if (ImGui::Button(LOC("close"), ImVec2(120, 0))) {
                strcpy_s(darFilter, "");
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // Function to split a comma-separated keyword string into a vector
    std::vector<std::string> SplitKeywords(const std::string& s) {
        std::vector<std::string> tokens;
        std::string token;
        std::istringstream tokenStream(s);
        while (std::getline(tokenStream, token, ',')) {
            // Remove whitespace before and after the token
            token.erase(0, token.find_first_not_of(" \t\n\r"));
            token.erase(token.find_last_not_of(" \t\n\r") + 1);
            if (!token.empty()) {
                tokens.push_back(token);
            }
        }
        return tokens;
    }

    void AnimationManager::SaveCustomCategories() {
        const std::filesystem::path categoriesPath = "Data/SKSE/Plugins/CycleMovesets/Categories";

        // 1. Ensure the save directory exists
        try {
            if (!std::filesystem::exists(categoriesPath)) {
                std::filesystem::create_directories(categoriesPath);
            }
        } catch (const std::filesystem::filesystem_error& e) {
            SKSE::log::error("Failed to create the categories directory: {}. Error: {}", categoriesPath.string(), e.what());
            return;
        }

        SKSE::log::info("Saving custom categories to individual files...");
        std::set<std::filesystem::path> savedFilePaths;

        if (std::filesystem::exists(categoriesPath)) {
            for (const auto& entry : std::filesystem::directory_iterator(categoriesPath)) {
                if (entry.is_regular_file() && entry.path().extension() == ".json") {
                    savedFilePaths.insert(entry.path());
                }
            }
        }

        // 2. Save each custom category to its own file
        for (const auto& pair : _categories) {
            const WeaponCategory& category = pair.second;
            if (category.isCustom) {
                rapidjson::Document doc;
                doc.SetObject();  // The file will contain a single object
                auto& allocator = doc.GetAllocator();

                // Populate the JSON object with the category data
                doc.AddMember("name", rapidjson::Value(category.name.c_str(), allocator), allocator);
                doc.AddMember("baseCategoryName", rapidjson::Value(category.baseCategoryName.c_str(), allocator),
                              allocator);
                doc.AddMember("isDualWield", category.isDualWield, allocator);
                doc.AddMember("isShieldCategory", category.isShieldCategory, allocator);

                rapidjson::Value keywordsArray(rapidjson::kArrayType);
                for (const auto& kw : category.keywords) {
                    keywordsArray.PushBack(rapidjson::Value(kw.c_str(), allocator), allocator);
                }
                doc.AddMember("keywords", keywordsArray, allocator);

                if (category.isDualWield) {
                    std::string leftHandBaseName = "Unarmed";  // Default
                    for (const auto& basePair : _categories) {
                        if (!basePair.second.isCustom &&
                            basePair.second.equippedTypeValue == category.leftHandEquippedTypeValue &&
                            basePair.second.leftHandEquippedTypeValue == category.leftHandEquippedTypeValue) {
                            leftHandBaseName = basePair.second.name;
                            break;
                        }
                    }
                    doc.AddMember("leftHandBaseCategoryName", rapidjson::Value(leftHandBaseName.c_str(), allocator),
                                  allocator);

                    rapidjson::Value leftKeywordsArray(rapidjson::kArrayType);
                    for (const auto& kw : category.leftHandKeywords) {
                        leftKeywordsArray.PushBack(rapidjson::Value(kw.c_str(), allocator), allocator);
                    }
                    doc.AddMember("leftHandKeywords", leftKeywordsArray, allocator);
                }

                // Define the file path and save it
                std::filesystem::path categoryFilePath = categoriesPath / (category.name + ".json");
                std::ofstream ofs(categoryFilePath);
                if (ofs) {
                    rapidjson::StringBuffer buffer;
                    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
                    doc.Accept(writer);
                    ofs << buffer.GetString();
                    ofs.close();
                    savedFilePaths.insert(categoryFilePath);  // Add to the list of valid files
                } else {
                    SKSE::log::error("Failed to open {} for writing!", categoryFilePath.string());
                }
            }
        }
        std::set<std::filesystem::path> currentCustomCategoryFiles;
        for (const auto& pair : _categories) {
            if (pair.second.isCustom) {
                currentCustomCategoryFiles.insert(categoriesPath / (pair.first + ".json"));
            }
        }
        // 3. Remove orphaned files (from categories that were deleted in the UI)
        for (const auto& existingPath : savedFilePaths) {
            if (currentCustomCategoryFiles.find(existingPath) == currentCustomCategoryFiles.end()) {
                SKSE::log::info("Removing orphaned category file: {}", existingPath.string());
                std::filesystem::remove(existingPath);
            }
        }
    }

    void AnimationManager::LoadCustomCategories() {
        const std::filesystem::path categoriesPath = "Data/SKSE/Plugins/CycleMovesets/Categories";
        if (!std::filesystem::exists(categoriesPath)) {
            SKSE::log::info("Custom categories directory not found. Skipping.");
            return;
        }

        SKSE::log::info("Loading custom categories from individual files...");

        std::map<std::string, const WeaponCategory*> baseCategories;
        for (const auto& pair : _categories) {
            if (!pair.second.isCustom) {
                baseCategories[pair.second.name] = &pair.second;
            }
        }

        // Iterate over each file in the categories directory
        for (const auto& entry : std::filesystem::directory_iterator(categoriesPath)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") {
                continue;
            }

            std::ifstream ifs(entry.path());
            if (!ifs) {
                SKSE::log::error("Failed to open category file: {}", entry.path().string());
                continue;
            }

            std::string jsonContent((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            ifs.close();

            rapidjson::Document doc;
            doc.Parse(jsonContent.c_str());

            if (doc.HasParseError() || !doc.IsObject()) {
                SKSE::log::error("JSON parse error or the file is not an object for: {}", entry.path().string());
                continue;
            }

            const rapidjson::Value& categoryObj = doc;  // The root document is the object itself

            // The rest of the reading logic is the same, but using 'categoryObj'
            if (!categoryObj.HasMember("name") || !categoryObj["name"].IsString() ||
                !categoryObj.HasMember("baseCategoryName") || !categoryObj["baseCategoryName"].IsString() ||
                !categoryObj.HasMember("isDualWield") || !categoryObj["isDualWield"].IsBool() ||
                !categoryObj.HasMember("keywords") || !categoryObj["keywords"].IsArray()) {
                SKSE::log::warn("Malformed custom category object or missing fields in {}. Skipping.",
                                entry.path().string());
                continue;
            }

            std::string name = categoryObj["name"].GetString();
            std::string baseName = categoryObj["baseCategoryName"].GetString();

            auto it = baseCategories.find(baseName);
            if (it == baseCategories.end()) {
                SKSE::log::warn("Base category '{}' for '{}' not found. Skipping.", baseName, name);
                continue;
            }
            const WeaponCategory* baseCat = it->second;

            WeaponCategory newCat;
            newCat.name = name;
            newCat.isCustom = true;
            newCat.baseCategoryName = baseName;
            newCat.equippedTypeValue = baseCat->equippedTypeValue;
            newCat.isDualWield = categoryObj["isDualWield"].GetBool();
            if (categoryObj.HasMember("isShieldCategory") && categoryObj["isShieldCategory"].IsBool()) {
                newCat.isShieldCategory = categoryObj["isShieldCategory"].GetBool();
            } else {
                newCat.isShieldCategory = false;  // Default value if not present in the file
            }

            for (const auto& kw : categoryObj["keywords"].GetArray()) {
                if (kw.IsString()) newCat.keywords.push_back(kw.GetString());
            }

            if (newCat.isDualWield) {
                if (!categoryObj.HasMember("leftHandBaseCategoryName") ||
                    !categoryObj["leftHandBaseCategoryName"].IsString() || !categoryObj.HasMember("leftHandKeywords") ||
                    !categoryObj["leftHandKeywords"].IsArray()) {
                    SKSE::log::warn("Dual category '{}' doesn't have left-hand fields. Skipping.", name);
                    continue;
                }
                std::string leftHandBaseName = categoryObj["leftHandBaseCategoryName"].GetString();
                auto itLeft = baseCategories.find(leftHandBaseName);
                if (itLeft != baseCategories.end()) {
                    newCat.leftHandEquippedTypeValue = itLeft->second->equippedTypeValue;
                } else {
                    newCat.leftHandEquippedTypeValue = 0.0;
                }
                for (const auto& kw : categoryObj["leftHandKeywords"].GetArray()) {
                    if (kw.IsString()) newCat.leftHandKeywords.push_back(kw.GetString());
                }
            } else {
                newCat.leftHandEquippedTypeValue = baseCat->leftHandEquippedTypeValue;
            }

            for (int i = 0; i < 4; ++i) {
                std::string defaultName = std::format("Stance {}", i + 1);
                newCat.stanceNames[i] = defaultName;
                strcpy_s(newCat.stanceNameBuffers[i].data(), newCat.stanceNameBuffers[i].size(), defaultName.c_str());
            }

            _categories[newCat.name] = newCat;
        }
    }

    void AnimationManager::DrawCreateCategoryModal() {
        // Determine whether we are in edit mode based on the pointer
        bool isEditing = (_categoryToEditPtr != nullptr);
        const char* popupTitle = isEditing ? "Edit Custom Category" : "Create New Category";

        if (_isCreateCategoryModalOpen) {
            ImGui::OpenPopup(popupTitle);
            _isCreateCategoryModalOpen = false;
        }

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 center = ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f);
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        if (ImGui::BeginPopupModal(popupTitle, NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            // --- Lists of base categories for the combos ---
            std::vector<const char*> baseCategoryNames;
            std::vector<const WeaponCategory*> baseCategoryPtrs;
            for (const auto& pair : _categories) {
                if (!pair.second.isCustom && !pair.second.isDualWield && !pair.second.isShieldCategory) {
                    baseCategoryNames.push_back(pair.first.c_str());
                    baseCategoryPtrs.push_back(&pair.second);
                }
            }
            std::vector<const char*> dualCategoryNames;
            std::vector<const WeaponCategory*> dualCategoryPtrs;
            for (const auto& pair : _categories) {
                if (!pair.second.isCustom) {
                    dualCategoryNames.push_back(pair.first.c_str());
                    dualCategoryPtrs.push_back(&pair.second);
                }
            }

            // --- Form fields ---
            ImGui::InputText("Category Name", _newCategoryNameBuffer, sizeof(_newCategoryNameBuffer));
            ImGui::Combo("Base Weapon (Right Hand)", &_newCategoryBaseIndex, baseCategoryNames.data(),
                         baseCategoryNames.size());
            ImGui::InputText("Keywords (comma-separated)", _newCategoryKeywordsBuffer,
                             sizeof(_newCategoryKeywordsBuffer));

            if (ImGui::Checkbox("Is Dual Wield", &_newCategoryIsDual)) {
                if (_newCategoryIsDual) _newCategoryIsShield = false;
            }
            ImGui::SameLine();
            if (ImGui::Checkbox("Left Hand is Shield", &_newCategoryIsShield)) {
                if (_newCategoryIsShield) _newCategoryIsDual = false;
            }

            if (_newCategoryIsDual) {
                ImGui::Separator();
                ImGui::Text("Dual Wield Options");
                ImGui::Combo("Base Weapon (Left Hand)", &_newCategoryLeftHandBaseIndex, baseCategoryNames.data(),
                             baseCategoryNames.size());
                ImGui::InputText("Left Hand Keywords", _newCategoryLeftHandKeywordsBuffer,
                                 sizeof(_newCategoryLeftHandKeywordsBuffer));
            }
            ImGui::Separator();

            // ============================ START OF FIXED SAVE LOGIC ============================
            const char* saveButtonText = LOC("save");
            if (ImGui::Button(saveButtonText, ImVec2(120, 0))) {
                std::string newName = _newCategoryNameBuffer;

                // Unified validation: checks if the name is empty or already exists (considering whether it's edit or create)
                if (newName.empty() || (!isEditing && _categories.count(newName)) ||
                    (isEditing && newName != _originalCategoryName && _categories.count(newName))) {
                    RE::DebugNotification("ERROR: Category name cannot be empty or already exists!");
                } else {
                    // --- EDIT PATH ---
                    if (isEditing) {
                        std::string originalName = _originalCategoryName;
                        bool nameChanged = (newName != originalName);

                        if (nameChanged) {
                            // 1. Rename configuration files
                            try {
                                const std::filesystem::path categoriesPath =
                                    "Data/SKSE/Plugins/CycleMovesets/Categories";
                                const std::filesystem::path stancesPath = "Data/SKSE/Plugins/CycleMovesets/Stances";

                                std::filesystem::path oldCatFile = categoriesPath / (originalName + ".json");
                                if (std::filesystem::exists(oldCatFile)) {
                                    std::filesystem::rename(oldCatFile, categoriesPath / (newName + ".json"));
                                }

                                std::filesystem::path oldStanceFile = stancesPath / (originalName + ".json");
                                if (std::filesystem::exists(oldStanceFile)) {
                                    std::filesystem::rename(oldStanceFile, stancesPath / (newName + ".json"));
                                }
                            } catch (const std::filesystem::filesystem_error& e) {
                                SKSE::log::error("Failed to rename files for category '{}': {}", originalName,
                                                 e.what());
                            }

                            // 2. Move the in-memory object to the new key (preserves movesets and stances)
                            auto nodeHandle = _categories.extract(originalName);
                            if (!nodeHandle.empty()) {
                                nodeHandle.key() = newName;
                                nodeHandle.mapped().name = newName;
                                _categories.insert(std::move(nodeHandle));
                            }
                            auto npcNodeHandle = _npcCategories.extract(originalName);
                            if (!npcNodeHandle.empty()) {
                                npcNodeHandle.key() = newName;
                                npcNodeHandle.mapped().name = newName;
                                _npcCategories.insert(std::move(npcNodeHandle));
                            }
                        }

                        // 3. Update the category properties (now under the correct name)
                        WeaponCategory& catToUpdate =
                            _categories.at(newName);  // .at() is safe here since the object already exists
                        catToUpdate.isDualWield = _newCategoryIsDual;
                        catToUpdate.isShieldCategory = _newCategoryIsShield;
                        const WeaponCategory* baseCat = baseCategoryPtrs[_newCategoryBaseIndex];
                        catToUpdate.baseCategoryName = baseCat->name;
                        catToUpdate.keywords = SplitKeywords(_newCategoryKeywordsBuffer);

                        if (catToUpdate.isShieldCategory) {
                            catToUpdate.equippedTypeValue = baseCat->equippedTypeValue;
                            catToUpdate.leftHandEquippedTypeValue = 11.0;
                        } else if (catToUpdate.isDualWield) {
                            const WeaponCategory* leftBaseCat = dualCategoryPtrs[_newCategoryLeftHandBaseIndex];
                            catToUpdate.equippedTypeValue = baseCat->equippedTypeValue;
                            catToUpdate.leftHandEquippedTypeValue = leftBaseCat->equippedTypeValue;
                            catToUpdate.leftHandKeywords = SplitKeywords(_newCategoryLeftHandKeywordsBuffer);
                        } else {
                            catToUpdate.equippedTypeValue = baseCat->equippedTypeValue;
                            catToUpdate.leftHandEquippedTypeValue = baseCat->leftHandEquippedTypeValue;
                        }

                        // Synchronize with NPCs
                        _npcCategories.at(newName) = catToUpdate;

                        // --- CREATE PATH (THE MAIN FIX) ---
                    } else {
                        // 1. Create the new category using the [] operator
                        WeaponCategory& newCat = _categories[newName];  // <--- FIX: Uses [] to create
                        newCat.name = newName;
                        newCat.isCustom = true;
                        newCat.isDualWield = _newCategoryIsDual;
                        newCat.isShieldCategory = _newCategoryIsShield;

                        // 2. Populate all properties of the newly created category
                        const WeaponCategory* baseCat = baseCategoryPtrs[_newCategoryBaseIndex];
                        newCat.baseCategoryName = baseCat->name;
                        newCat.keywords = SplitKeywords(_newCategoryKeywordsBuffer);

                        if (newCat.isShieldCategory) {
                            newCat.equippedTypeValue = baseCat->equippedTypeValue;
                            newCat.leftHandEquippedTypeValue = 11.0;
                        } else if (newCat.isDualWield) {
                            const WeaponCategory* leftBaseCat = dualCategoryPtrs[_newCategoryLeftHandBaseIndex];
                            newCat.equippedTypeValue = baseCat->equippedTypeValue;
                            newCat.leftHandEquippedTypeValue = leftBaseCat->equippedTypeValue;
                            newCat.leftHandKeywords = SplitKeywords(_newCategoryLeftHandKeywordsBuffer);
                        } else {
                            newCat.equippedTypeValue = baseCat->equippedTypeValue;
                            newCat.leftHandEquippedTypeValue = baseCat->leftHandEquippedTypeValue;
                        }

                        // 3. Initialize the default stance names for the new category
                        for (int i = 0; i < 4; ++i) {
                            std::string defaultName = std::format("Stance {}", i + 1);
                            newCat.stanceNames[i] = defaultName;
                            strcpy_s(newCat.stanceNameBuffers[i].data(), newCat.stanceNameBuffers[i].size(),
                                     defaultName.c_str());
                        }

                        // 4. Synchronize the new category with the NPC list
                        _npcCategories[newName] = newCat;
                    }

                    // Finalize and close the modal
                    _categoryToEditPtr = nullptr;
                    ImGui::CloseCurrentPopup();
                }
            }
            // ============================ END OF FIXED SAVE LOGIC ============================

            ImGui::SameLine();
            if (ImGui::Button(LOC("close"), ImVec2(120, 0))) {
                _categoryToEditPtr = nullptr;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        } else {
            // Ensure the edit pointer is cleared if the popup is closed some other way
            if (isEditing) {
                _categoryToEditPtr = nullptr;
            }
        }
    }

    void AnimationManager::DrawCategoryManager() {
        bool isEditing = (_categoryToEditPtr != nullptr);
        const char* popupTitle = isEditing ? "Edit Custom Category" : "Create New Category";

        if (_isCreateCategoryModalOpen) {
            ImGui::OpenPopup(popupTitle);
            _isCreateCategoryModalOpen = false;
        }

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 center = ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f);
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        if (ImGui::Button("Create New Category")) {
            _categoryToEditPtr = nullptr;  // Ensures we are in create mode
            // Clear the buffers for a new form
            strcpy_s(_originalCategoryName, "");
            strcpy_s(_newCategoryNameBuffer, "");
            strcpy_s(_newCategoryKeywordsBuffer, "");
            strcpy_s(_newCategoryLeftHandKeywordsBuffer, "");
            _newCategoryBaseIndex = 0;
            _newCategoryLeftHandBaseIndex = 0;
            _newCategoryIsDual = false;
            _newCategoryIsShield = false;
            _isCreateCategoryModalOpen = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Create new weapon categories based on vanilla types, but with specific keywords.");
        }

        ImGui::Separator();

        if (ImGui::BeginTable("CategoriesTable", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Category Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableHeadersRow();

            std::string categoryToDelete;

            // We use an iterator so we can safely erase from the list
            for (auto it = _categories.begin(); it != _categories.end();) {
                auto& [name, category] = *it;

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%s", name.c_str());

                ImGui::TableNextColumn();
                if (category.isCustom) {
                    ImGui::Text("Base: %s", category.baseCategoryName.c_str());
                } else {
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Base Category");
                }

                ImGui::TableNextColumn();
                if (category.isCustom) {
                    ImGui::PushID(name.c_str());
                    if (ImGui::Button("Edit")) {
                        _categoryToEditPtr = &category;  // Points to the category, activating edit mode

                        // Fill the buffers with the category's current data
                        strcpy_s(_originalCategoryName, name.c_str());  // Store the original name
                        strcpy_s(_newCategoryNameBuffer, name.c_str());
                        _newCategoryIsDual = category.isDualWield;
                        _newCategoryIsShield = category.isShieldCategory;

                        // Convert keyword vectors to strings
                        auto join_keywords = [](const std::vector<std::string>& keywords) {
                            std::string result;
                            for (size_t i = 0; i < keywords.size(); ++i) {
                                result += keywords[i] + (i == keywords.size() - 1 ? "" : ", ");
                            }
                            return result;
                        };
                        strcpy_s(_newCategoryKeywordsBuffer, join_keywords(category.keywords).c_str());
                        strcpy_s(_newCategoryLeftHandKeywordsBuffer, join_keywords(category.leftHandKeywords).c_str());

                        // --- START OF FIX #3: FIND CORRECT INDICES ---
                        // Logic to find the right-hand base index
                        _newCategoryBaseIndex = 0;
                        int current_idx = 0;
                        for (const auto& pair : _categories) {
                            if (!pair.second.isCustom && !pair.second.isDualWield) {
                                if (pair.first == category.baseCategoryName) {
                                    _newCategoryBaseIndex = current_idx;
                                    break;
                                }
                                current_idx++;
                            }
                        }

                        // Logic to find the left-hand base index (if dual wield)
                        _newCategoryLeftHandBaseIndex = 0;
                        if (category.isDualWield) {
                            current_idx = 0;
                            for (const auto& pair : _categories) {
                                if (!pair.second.isCustom) {
                                    // We need to find the name of the left-hand base category
                                    // This logic is complex; for now we'll search by typeValue
                                    if (pair.second.equippedTypeValue == category.leftHandEquippedTypeValue &&
                                        pair.second.leftHandEquippedTypeValue == category.leftHandEquippedTypeValue) {
                                        _newCategoryLeftHandBaseIndex = current_idx;
                                        break;
                                    }
                                    current_idx++;
                                }
                            }
                        }
                        // --- END OF FIX #3 ---

                        _isCreateCategoryModalOpen = true;  // Opens the same modal, but now in edit mode
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Delete")) {
                        categoryToDelete = name;
                    }
                    ImGui::PopID();
                }
                ++it;
            }
            ImGui::EndTable();

            if (!categoryToDelete.empty()) {
                _categories.erase(categoryToDelete);
                _npcCategories.erase(categoryToDelete);
                SKSE::log::info("Category '{}' removed.", categoryToDelete);
            }
        }
    }

    void AnimationManager::AddCompareEquippedTypeCondition(rapidjson::Value& conditionsArray, double type, bool isLeftHand,
                                                           rapidjson::Document::AllocatorType& allocator) {
        rapidjson::Value equippedType(rapidjson::kObjectType);
        equippedType.AddMember("condition", "IsEquippedType", allocator);
        rapidjson::Value typeVal(rapidjson::kObjectType);
        typeVal.AddMember("value", type, allocator);
        equippedType.AddMember("Type", typeVal, allocator);
        equippedType.AddMember("Left hand", isLeftHand, allocator);
        conditionsArray.PushBack(equippedType, allocator);
    }

    void AnimationManager::AddShieldCategoryExclusions(rapidjson::Value& parentArray,
                                                       rapidjson::Document::AllocatorType& allocator) {
        // 1. Collect all keywords from custom shield categories
        std::vector<std::string> competingKeywords;
        for (const auto& pair : _categories) {
            const WeaponCategory& otherCategory = pair.second;
            // The condition is: be custom, be a shield category, AND have keywords
            if (otherCategory.isCustom && otherCategory.isShieldCategory && !otherCategory.keywords.empty()) {
                competingKeywords.insert(competingKeywords.end(), otherCategory.keywords.begin(),
                                         otherCategory.keywords.end());
            }
        }

        if (competingKeywords.empty()) {
            return;  // No exclusion necessary
        }

        // 2. Create a single AND block to contain all exclusions (NOT keyword1 AND NOT keyword2 ...)
        rapidjson::Value exclusionAndBlock(rapidjson::kObjectType);
        exclusionAndBlock.AddMember("condition", "AND", allocator);
        exclusionAndBlock.AddMember("comment", "Exclude competing custom Shield + Weapon categories", allocator);
        rapidjson::Value innerExclusionConditions(rapidjson::kArrayType);

        for (const auto& keyword : competingKeywords) {
            // Add the negated 'IsEquippedHasKeyword' condition for the right hand (where the weapon with keyword is)
            AddKeywordCondition(innerExclusionConditions, keyword, false, true, allocator);
        }

        exclusionAndBlock.AddMember("Conditions", innerExclusionConditions, allocator);
        parentArray.PushBack(exclusionAndBlock, allocator);
    }

    void AnimationManager::PopulateNpcList() {
        SKSE::log::info("Starting scan of all NPCs...");
        _fullNpcList.clear();
        _pluginList.clear();
        std::set<std::string> uniquePlugins;

        auto dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) {
            SKSE::log::error("Failed to get the TESDataHandler.");
            return;
        }

        const auto& npcArray = dataHandler->GetFormArray<RE::TESNPC>();
        for (const auto& npc : npcArray) {
            if (npc && !npc->IsPlayer() && npc->GetFile(0)) {
                NPCInfo info;
                info.formID = npc->GetFormID();

                // --- START OF CHANGE ---
                // Replaced npc->GetFormEditorID() with the clib_util call for more robustness.
                info.editorID = clib_util::editorID::get_editorID(npc);
                // --- END OF CHANGE ---

                const char* npcName = npc->GetName();
                info.name = (npcName) ? npcName : "";

                auto plugin = npc->GetFile(0)->GetFilename();
                info.pluginName = std::string(plugin);

                _fullNpcList.push_back(info);
                uniquePlugins.insert(info.pluginName);
            }
        }

        _pluginList.push_back(LOC("all"));
        for (const auto& pluginName : uniquePlugins) {
            _pluginList.push_back(pluginName);
        }

        _npcListPopulated = true;
        SKSE::log::info("Scan complete. {} NPCs loaded from {} plugins.", _fullNpcList.size(),
                        uniquePlugins.size());
    }

    void AnimationManager::DrawNpcSelectionModal() {
        if (_isNpcSelectionModalOpen) {
            ImGui::OpenPopup("Selector");
        }

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 center = ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f);
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(viewport->Size.x * 0.7f, viewport->Size.y * 0.7f));

        if (ImGui::BeginPopupModal("Selector", &_isNpcSelectionModalOpen, ImGuiWindowFlags_None)) {
            // --- LOG 1: Check if the main data lists are loaded ---
            if (ImGui::IsWindowAppearing()) {  // Log only the first time the modal opens
                SKSE::log::info(
                    "[NpcSelectionModal] Opening modal. List sizes: NPCs={}, Factions={}, Keywords={}, "
                    "Races={}",
                    _fullNpcList.size(), _allFactions.size(), _allKeywords.size(), _allRaces.size());
            }

            const char* title = "Select one";
            switch (_ruleTypeToCreate) {
                case RuleType::UniqueNPC:
                    title = "NPC";
                    break;
                case RuleType::Keyword:
                    title = "Keyword";
                    break;
                case RuleType::Faction:
                    title = "Faction";
                    break;
                case RuleType::Race:
                    title = "Race";
                    break;
            }
            ImGui::Text("%s", title);
            ImGui::Separator();

            static char filterBuffer[128] = "";
            ImGui::InputText(LOC("filter"), filterBuffer, sizeof(filterBuffer));
            ImGui::SameLine();
            std::vector<const char*> pluginNamesCStr;
            for (const auto& name : _pluginList) {
                pluginNamesCStr.push_back(name.c_str());
            }
            ImGui::PushItemWidth(200);
            ImGui::Combo("Plugin", &_selectedPluginIndex, pluginNamesCStr.data(), pluginNamesCStr.size());
            ImGui::PopItemWidth();
            ImGui::Separator();

            if (ImGui::BeginTable("SelectionTable", 4,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("EditorID / ID", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Plugin", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Add", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableHeadersRow();

                std::string filterTextLower = filterBuffer;
                std::transform(filterTextLower.begin(), filterTextLower.end(), filterTextLower.begin(), ::tolower);
                std::string selectedPlugin = (_pluginList.empty() || _selectedPluginIndex >= _pluginList.size())
                                                 ? ""
                                                 : _pluginList[_selectedPluginIndex];

                // --- LOG 2: Check the current filters ---
                //SKSE::log::info("[NpcSelectionModal] Active Filters -> Text: '{}', Plugin: '{}' (index {})",filterTextLower, selectedPlugin, _selectedPluginIndex);

                switch (_ruleTypeToCreate) {
                    case RuleType::UniqueNPC: {
                        std::vector<int> filtered_indices;
                        filtered_indices.reserve(_fullNpcList.size());
                        for (int i = 0; i < _fullNpcList.size(); ++i) {
                            const auto& npc = _fullNpcList[i];
                            if (_selectedPluginIndex != 0 && npc.pluginName != selectedPlugin) continue;
                            std::string nameLower = npc.name;
                            std::string editorIdLower = npc.editorID;
                            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
                            std::transform(editorIdLower.begin(), editorIdLower.end(), editorIdLower.begin(),
                                           ::tolower);
                            if (filterTextLower.empty() || nameLower.find(filterTextLower) != std::string::npos ||
                                editorIdLower.find(filterTextLower) != std::string::npos) {
                                filtered_indices.push_back(i);
                            }
                        }

                        // ====================== START OF MANUAL CLIPPER ======================
                        // 1. Get the height of a single table row.
                        const float item_height = ImGui::GetTextLineHeightWithSpacing();

                        // 2. Get the current scroll position and the visible area height.
                        const float scroll_y = ImGui::GetScrollY();
                        ImVec2 content_avail;
                        ImGui::GetContentRegionAvail(&content_avail);
                        const float content_height = content_avail.y;

                        // 3. Calculate the index of the first and last items to render.
                        int display_start = static_cast<int>(scroll_y / item_height);
                        int display_end = display_start + static_cast<int>(ceil(content_height / item_height)) + 1;

                        // 4. Ensure the indices don't go outside the bounds of our list.
                        display_start = std::max(0, display_start);
                        display_end = std::min(static_cast<int>(filtered_indices.size()), display_end);

                        // Log to verify our manual calculations


                        // Add space at the top to simulate scrolling
                        ImGui::Dummy(ImVec2(0.0f, display_start * item_height));
                        for (int i = display_start; i < display_end; i++) {
                            const auto& npc = _fullNpcList[filtered_indices[i]];
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::Text("%s", npc.name.c_str());
                            ImGui::TableNextColumn();
                            ImGui::Text("%s", npc.editorID.c_str());
                            ImGui::TableNextColumn();
                            ImGui::Text("%s", npc.pluginName.c_str());
                            ImGui::TableNextColumn();
                            ImGui::PushID(npc.formID);
                            if (ImGui::Button("Select")) {
                                MovesetRule newRule;
                                newRule.type = RuleType::UniqueNPC;
                                newRule.displayName = npc.name;
                                newRule.identifier = std::format("{:08X}", npc.formID);
                                newRule.pluginName = npc.pluginName;
                                newRule.formID = npc.formID;
                                newRule.categories = _categories;
                                for (auto& pair : newRule.categories) {
                                    pair.second.instances[0].modInstances.clear();
                                }
                                _npcRules.push_back(newRule);
                                _isNpcSelectionModalOpen = false;
                            }
                            ImGui::PopID();
                        }
                        ImGui::Dummy(ImVec2(0.0f, (filtered_indices.size() - display_end) * item_height));
                        break;
                    }
                    case RuleType::Faction:
                    case RuleType::Keyword:
                    case RuleType::Race: {
                        auto draw_list_with_manual_clipper = [&](auto& info_list) {
                            // STEP 1: Filtering
                            std::vector<int> filtered_indices;
                            filtered_indices.reserve(info_list.size());
                            for (int i = 0; i < info_list.size(); ++i) {
                                const auto& info = info_list[i];
                                if (_selectedPluginIndex != 0 && info.pluginName != selectedPlugin) continue;

                                std::string name_lower;
                                if constexpr (std::is_same_v<std::decay_t<decltype(info)>, RaceInfo>) {
                                    name_lower = info.fullName;
                                } else {
                                    name_lower = info.editorID;
                                }
                                std::string editorid_lower = info.editorID;
                                std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
                                std::transform(editorid_lower.begin(), editorid_lower.end(), editorid_lower.begin(),
                                               ::tolower);

                                if (filterTextLower.empty() || name_lower.find(filterTextLower) != std::string::npos ||
                                    editorid_lower.find(filterTextLower) != std::string::npos) {
                                    filtered_indices.push_back(i);
                                }
                            }

                            // STEP 2: Manual Clipper
                            const float item_height = ImGui::GetTextLineHeightWithSpacing();
                            const float scroll_y = ImGui::GetScrollY();
                            ImVec2 content_avail;
                            ImGui::GetContentRegionAvail(&content_avail);
                            const float content_height = content_avail.y;
                            int display_start = static_cast<int>(scroll_y / item_height);
                            int display_end = display_start + static_cast<int>(ceil(content_height / item_height)) + 1;
                            display_start = std::max(0, display_start);
                            display_end = std::min(static_cast<int>(filtered_indices.size()), display_end);

                            ImGui::Dummy(ImVec2(0.0f, display_start * item_height));

                            // STEP 3: Rendering
                            for (int i = display_start; i < display_end; i++) {
                                const auto& info = info_list[filtered_indices[i]];
                                ImGui::TableNextRow();
                                if constexpr (std::is_same_v<std::decay_t<decltype(info)>, RaceInfo>) {
                                    ImGui::TableNextColumn();
                                    ImGui::Text("%s", info.fullName.c_str());
                                } else {
                                    ImGui::TableNextColumn();
                                    ImGui::Text("%s", info.editorID.c_str());
                                }
                                ImGui::TableNextColumn();
                                ImGui::Text("%s", info.editorID.c_str());
                                ImGui::TableNextColumn();
                                ImGui::Text("%s", info.pluginName.c_str());
                                ImGui::TableNextColumn();
                                ImGui::PushID(info.formID);
                                if (ImGui::Button("Select")) {
                                    MovesetRule newRule;
                                    newRule.type = _ruleTypeToCreate;
                                    newRule.displayName = info.editorID;
                                    newRule.identifier = info.editorID;
                                    newRule.pluginName = info.pluginName;
                                    newRule.formID = info.formID;
                                    newRule.categories = _categories;
                                    for (auto& pair : newRule.categories) {
                                        pair.second.instances[0].modInstances.clear();
                                    }
                                    _npcRules.push_back(newRule);
                                    _isNpcSelectionModalOpen = false;
                                }
                                ImGui::PopID();
                            }
                            ImGui::Dummy(ImVec2(0.0f, (filtered_indices.size() - display_end) * item_height));
                        };

                        if (_ruleTypeToCreate == RuleType::Faction)
                            draw_list_with_manual_clipper(_allFactions);
                        else if (_ruleTypeToCreate == RuleType::Keyword)
                            draw_list_with_manual_clipper(_allKeywords);
                        else if (_ruleTypeToCreate == RuleType::Race)
                            draw_list_with_manual_clipper(_allRaces);

                        break;
                    }
                }
                ImGui::EndTable();
            }

            /*ImGui::Separator();
            if (ImGui::Button("Close", ImVec2(120, 0))) {
                _isNpcSelectionModalOpen = false;
            }*/
            ImGui::EndPopup();
        }
    }

    void AnimationManager::PopulateHkxFiles(CreatorSubAnimationInstance& instance) {
        if (!instance.sourceDef) return;

        // Ensure the path is a directory
        std::filesystem::path sourceDirectory = instance.sourceDef->path;
        if (std::filesystem::is_regular_file(sourceDirectory)) {
            sourceDirectory = sourceDirectory.parent_path();
        }

        if (!std::filesystem::exists(sourceDirectory) || !std::filesystem::is_directory(sourceDirectory)) {
            return;
        }

        instance.hkxFileSelection.clear();
        for (const auto& fileEntry : std::filesystem::directory_iterator(sourceDirectory)) {
            if (fileEntry.is_regular_file()) {
                std::string extension = fileEntry.path().extension().string();
                std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
                if (extension == ".hkx") {
                    // Add the file to the list, selected by default
                    instance.hkxFileSelection[fileEntry.path().filename().string()] = true;
                }
            }
        }
    }

    void AnimationManager::LoadGameDataForNpcRules() {
        //SKSE::log::info("Starting to load data (Factions, Keywords, Races) for the Rule Manager...");

        _allFactions.clear();
        _allKeywords.clear();
        _allRaces.clear();

        auto dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) {
            SKSE::log::error("Failed to get the TESDataHandler. Rule data loading was aborted.");
            return;
        }

        // Load Factions
        const auto& factions = dataHandler->GetFormArray<RE::TESFaction>();
        for (const auto* faction : factions) {
            if (faction && faction->GetFile(0)) {  // Added GetFile check
                std::string editorID = clib_util::editorID::get_editorID(faction);
                if (!editorID.empty()) {
                    const auto* plugin = faction->GetFile(0);
                    _allFactions.push_back({faction->GetFormID(), editorID, std::string(plugin->GetFilename())});
                }
            }
        }
        SKSE::log::info("Loaded {} factions.", _allFactions.size());

        // Load Keywords
        const auto& keywords = dataHandler->GetFormArray<RE::BGSKeyword>();
        for (const auto* keyword : keywords) {
            if (keyword && keyword->GetFile(0)) {  // Added GetFile check
                std::string editorID = clib_util::editorID::get_editorID(keyword);
                if (!editorID.empty()) {
                    const auto* plugin = keyword->GetFile(0);
                    _allKeywords.push_back({keyword->GetFormID(), editorID, std::string(plugin->GetFilename())});
                }
            }
        }
        SKSE::log::info("Loaded {} keywords.", _allKeywords.size());

        // Load Races
        const auto& races = dataHandler->GetFormArray<RE::TESRace>();
        for (const auto* race : races) {
            if (race && race->GetFile(0)) {  // Added GetFile check
                std::string editorID = clib_util::editorID::get_editorID(race);
                if (!editorID.empty() && editorID != "PlayerRace") {
                    const auto* plugin = race->GetFile(0);
                    _allRaces.push_back(
                        {race->GetFormID(), editorID, race->GetFullName(), std::string(plugin->GetFilename())});
                }
            }
        }
        SKSE::log::info("Loaded {} races (excluding 'PlayerRace').", _allRaces.size());
    }

    // Format the FormID to the 6-digit format that OAR expects, removing the plugin index
    std::string FormatFormIDForOAR(RE::FormID formID) { return std::format("{:06X}", formID & 0x00FFFFFF); }

    void AnimationManager::AddIsActorBaseCondition(rapidjson::Value& conditionsArray, const std::string& plugin,
                                                   RE::FormID formID, bool negated,
                                                   rapidjson::Document::AllocatorType& allocator) {
        rapidjson::Value condition(rapidjson::kObjectType);
        condition.AddMember("condition", "IsActorBase", allocator);
        if (negated) {
            condition.AddMember("negated", true, allocator);
        }
        rapidjson::Value params(rapidjson::kObjectType);
        params.AddMember("pluginName", rapidjson::Value(plugin.c_str(), allocator), allocator);
        params.AddMember("formID", rapidjson::Value(FormatFormIDForOAR(formID).c_str(), allocator), allocator);
        condition.AddMember("Actor base", params, allocator);
        conditionsArray.PushBack(condition, allocator);
    }

    void AnimationManager::AddIsInFactionCondition(rapidjson::Value& conditionsArray, const std::string& plugin,
                                                   RE::FormID formID, rapidjson::Document::AllocatorType& allocator) {
        rapidjson::Value condition(rapidjson::kObjectType);
        condition.AddMember("condition", "IsInFaction", allocator);
        rapidjson::Value params(rapidjson::kObjectType);
        params.AddMember("pluginName", rapidjson::Value(plugin.c_str(), allocator), allocator);
        params.AddMember("formID", rapidjson::Value(FormatFormIDForOAR(formID).c_str(), allocator), allocator);
        condition.AddMember("Faction", params, allocator);
        conditionsArray.PushBack(condition, allocator);
    }

    void AnimationManager::AddHasKeywordCondition(rapidjson::Value& conditionsArray, const std::string& plugin,
                                                  RE::FormID formID, rapidjson::Document::AllocatorType& allocator) {
        rapidjson::Value condition(rapidjson::kObjectType);
        condition.AddMember("condition", "HasKeyword", allocator);
        condition.AddMember("requiredVersion", "1.0.0.0", allocator);

        // Main object that will be the value of the "Keyword" key
        rapidjson::Value keywordObj(rapidjson::kObjectType);

        // Internal "form" object
        rapidjson::Value formObj(rapidjson::kObjectType);
        formObj.AddMember("pluginName", rapidjson::Value(plugin.c_str(), allocator), allocator);
        formObj.AddMember("formID", rapidjson::Value(FormatFormIDForOAR(formID).c_str(), allocator), allocator);

        // Add the "form" object inside the "Keyword" object
        keywordObj.AddMember("form", formObj, allocator);

        // Add the complete "Keyword" object to the main condition
        condition.AddMember("Keyword", keywordObj, allocator);

        conditionsArray.PushBack(condition, allocator);
    }

    void AnimationManager::AddIsRaceCondition(rapidjson::Value& conditionsArray, const std::string& plugin,
                                              RE::FormID formID, rapidjson::Document::AllocatorType& allocator) {
        rapidjson::Value condition(rapidjson::kObjectType);
        condition.AddMember("condition", "IsRace", allocator);
        rapidjson::Value params(rapidjson::kObjectType);
        params.AddMember("pluginName", rapidjson::Value(plugin.c_str(), allocator), allocator);
        params.AddMember("formID", rapidjson::Value(FormatFormIDForOAR(formID).c_str(), allocator), allocator);
        condition.AddMember("Race", params, allocator);
        conditionsArray.PushBack(condition, allocator);
    }

    int AnimationManager::GetPriorityForType(RuleType type) {
        switch (type) {
            case RuleType::UniqueNPC:
                return 4;
            case RuleType::Keyword:
                return 3;
            case RuleType::Faction:
                return 2;
            case RuleType::Race:
                return 1;
            case RuleType::GeneralNPC:
                return 0;
            default:
                return 0;
        }
    }

    NpcRuleMatch AnimationManager::FindBestMovesetConfiguration(RE::Actor* actor, const std::string& categoryName) {
        if (!actor) {
            // Return the general rule as default, even if empty
            SKSE::log::info("[FindBestMoveset] Null actor provided. Returning default general rule.");
            return {&_generalNpcRule, 0, GetPriorityForType(RuleType::GeneralNPC)};
        }
        //SKSE::log::info("=====================================================================");
        //SKSE::log::info("[FindBestMoveset] Starting search for actor: '{}' ({:08X}), Category: '{}'",actor->GetName(), actor->GetFormID(), categoryName);

        const std::vector<RuleType> priorityOrder = {RuleType::UniqueNPC, RuleType::Keyword, RuleType::Faction,
                                                     RuleType::Race};

        // Iterate through the priority order of rule TYPES
        for (const auto& typeToFind : priorityOrder) {
            //SKSE::log::info("[FindBestMoveset] Checking rules of type: {}", RuleTypeToString(typeToFind));
            // Iterate through the UI rule list (respecting the sub-priority of the list order)
            for (const auto& rule : _npcRules) {
                if (rule.type != typeToFind) continue;

                // Check if the rule applies to the actor
                bool match = false;
                switch (rule.type) {
                    case RuleType::UniqueNPC:
                        if (actor->GetActorBase()->GetFormID() == rule.formID) match = true;
                        break;
                    case RuleType::Keyword:
                        if (actor->GetActorBase()->HasKeywordString(rule.identifier)) match = true;
                        break;
                    case RuleType::Faction:
                        if (actor->GetActorBase()->IsInFaction(RE::TESForm::LookupByEditorID<RE::TESFaction>(rule.identifier)))
                            match = true;
                        break;
                    case RuleType::Race:
                        if (actor->GetActorBase()->GetRace() ==
                            RE::TESForm::LookupByEditorID<RE::TESRace>(rule.identifier))
                            match = true;
                        break;
                    default:
                        break;
                }

                if (match) {
                    auto category_it = rule.categories.find(categoryName);
                    if (category_it != rule.categories.end()) {
                        //SKSE::log::info("    [MATCH!] Rule '{}' applies to the actor.", rule.displayName);
                        const auto& category = category_it->second;
                        int count = 0;
                        // Calculate the count of selected movesets (still useful to have this info)
                        for (const auto& modInst : category.instances[0].modInstances) {
                            if (modInst.isSelected) count++;
                        }

                        if (count > 0) {
                            //SKSE::log::info("    -> Category has {} movesets. RETURNING THIS RULE.", count);
                            // HERE'S THE CHANGE: We return a pointer to the current rule (&rule)
                            return {&rule, count, GetPriorityForType(rule.type)};
                        }
                    }
                }
            }
        }

        // If no specific rule was found, use the General rule as fallback
        auto category_it = _generalNpcRule.categories.find(categoryName);
        if (category_it != _generalNpcRule.categories.end()) {
            int count = 0;
            for (const auto& modInst : category_it->second.instances[0].modInstances) {
                if (modInst.isSelected) count++;
            }
            // Return the pointer to the general rule
            return {&_generalNpcRule, count, GetPriorityForType(RuleType::GeneralNPC)};
        }

        // Final fallback: return the general rule even if it doesn't have the category
        return {&_generalNpcRule, 0, GetPriorityForType(RuleType::GeneralNPC)};
    }

    std::vector<int> AnimationManager::GetAvailableMovesetIndices(RE::Actor* actor, const std::string& categoryName) {
        if (!actor) return {};
        //SKSE::log::info("---------------------------------------------------------------------");
        //SKSE::log::info("[GetAvailableIndices] Looking up indices for actor: '{}', Category: '{}'", actor->GetName(),categoryName);

        // 1. Call the modified function to get the full "match"
        NpcRuleMatch match = FindBestMovesetConfiguration(actor, categoryName);
        actor->SetGraphVariableInt("CycleMovesetNpcType", match.priority);
        // 2. Access the rule pointer directly from the result
        const MovesetRule* rule = match.rule;

        // Safety measure, although the function should always return a valid pointer
        if (!rule) {
            SKSE::log::error("FindBestMovesetConfiguration unexpectedly returned a null pointer!");
            return {};
        }
        //SKSE::log::info("[GetAvailableIndices] Determined rule: '{}'", rule->displayName);
        // 2. Find the weapon category within the rule
        auto categoryIt = rule->categories.find(categoryName);
        if (categoryIt == rule->categories.end()) {
            //SKSE::log::warn("[GetAvailableIndices] Rule '{}' doesn't have category '{}'. Returning empty list.",rule->displayName, categoryName);
            return {};
        }
        const CategoryInstance& instance = categoryIt->second.instances[0];  // Stance 0 for NPCs

        // 3. Get the actor's current stats
        // *** FIXED HP CALCULATION ***
        float currentHealth = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
        float maxHealth = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kHealth);
        // Avoid division by zero if the actor has 0 max health for some reason
        float hpPercent = (maxHealth > 0) ? (currentHealth / maxHealth) * 100.0f : 0.0f;
        int level = actor->GetLevel();

        float currentStamina = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
        float maxStamina = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
        float stPercent = (maxStamina > 0) ? (currentStamina / maxStamina) * 100.0f : 0.0f;
        float currentMagicka = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kMagicka);
        float maxMagicka = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kMagicka);
        float mkPercent = (maxMagicka > 0) ? (currentMagicka / maxMagicka) * 100.0f : 0.0f;

        //SKSE::log::info("[GetAvailableIndices] Actor's current status -> HP: {:.2f} ({:.2f}/{:.2f}), Level: {}",hpPercent, currentHealth, maxHealth, level);
        std::vector<ScoredIndex> scoredCandidates;
        int currentPlaylistIndex = 1;

        for (const auto& modInst : instance.modInstances) {
            if (modInst.isSelected) {
                // Check if the conditions are met
                bool conditionsMet = (hpPercent <= modInst.hp && level >= modInst.level && stPercent <= modInst.st &&
                                      mkPercent <= modInst.mn);

                if (conditionsMet) {
                    // STEP 2: Calculate the "Proximity Score"
                    // The score is the total "distance" of the conditions. Lower is better.
                    float hp_distance = modInst.hp - hpPercent;  // E.g.: If HP is 40 and the condition is 50, the distance is 10.
                    float level_distance =
                        level - modInst.level;  // E.g.: If Lvl is 20 and the condition is 15, the distance is 5.
                    float st_distance = modInst.st - stPercent;
                    float mn_distance = modInst.mn - mkPercent;

                    float totalScore = hp_distance + level_distance + st_distance + mn_distance;

                    scoredCandidates.push_back({currentPlaylistIndex, totalScore});
                }
                currentPlaylistIndex++;
            }
        }

        // STEP 3: Sort candidates by score (lowest to highest)
        std::sort(scoredCandidates.begin(), scoredCandidates.end());

        // STEP 4: Extract only the sorted indices for the final result
        std::vector<int> availableIndices;
        availableIndices.reserve(scoredCandidates.size());
        for (const auto& candidate : scoredCandidates) {
            availableIndices.push_back(candidate.index);
        }

        // Final log with the results list
        std::string result_string = "[ ";
        for (int idx : availableIndices) {
            result_string += std::to_string(idx) + " ";
        }
        result_string += "]";
        //SKSE::log::info("[GetAvailableIndices] Filter complete. Returning available indices: {}", result_string);
        //SKSE::log::info("---------------------------------------------------------------------");

        return availableIndices;
    }

    void Settings::SyncMovementKeys() {
        keyForward = static_cast<uint32_t>(Settings::keyForward_k);
        keyBack = static_cast<uint32_t>(Settings::keyBack_k);
        keyLeft = static_cast<uint32_t>(Settings::keyLeft_k);
        keyRight = static_cast<uint32_t>(Settings::keyRight_k);
        SKSE::log::info("Movement keys synchronized for runtime.");
    }

    std::optional<std::pair<size_t, size_t>> AnimationManager::FindSubAnimationByPath(
        const std::filesystem::path& configPath) {
        for (size_t modIdx = 0; modIdx < _allMods.size(); ++modIdx) {
            const auto& mod = _allMods[modIdx];
            for (size_t subIdx = 0; subIdx < mod.subAnimations.size(); ++subIdx) {
                // Compare canonical paths to ensure consistency (ignores differences like / vs \)
                if (std::filesystem::equivalent(mod.subAnimations[subIdx].path, configPath)) {
                    return std::make_pair(modIdx, subIdx);
                }
            }
        }
        return std::nullopt;  // Not found
    }