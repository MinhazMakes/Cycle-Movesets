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


std::string PathToUTF8(const std::filesystem::path& path) {
    auto u8str = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8str.c_str()), u8str.length());
}

    // Função auxiliar para copiar um único arquivo com logs
    void CopySingleFile(const std::filesystem::path& sourceFile, const std::filesystem::path& destinationPath,
                        int& filesCopied) {
        try {
            std::filesystem::copy_file(sourceFile, destinationPath / sourceFile.filename(),
                                       std::filesystem::copy_options::overwrite_existing);
            filesCopied++;
        } catch (const std::filesystem::filesystem_error& e) {
            SKSE::log::error("Falha ao copiar arquivo: {}. Erro: {}", sourceFile.string(), e.what());
        }
    }

void ProcessCycleDarFile(const std::filesystem::path& cycleDarJsonPath) {
        SKSE::log::info("Processando CycleDar.json em: {}", cycleDarJsonPath.string());
        try {
            // 1. Abre e lê o arquivo JSON
            std::ifstream fileStream(cycleDarJsonPath);
            if (!fileStream) {
                SKSE::log::error("Falha ao abrir {}", cycleDarJsonPath.string());
                return;
            }
            std::string jsonContent((std::istreambuf_iterator<char>(fileStream)), std::istreambuf_iterator<char>());
            fileStream.close();

            // 2. Faz o parse do JSON
            rapidjson::Document doc;
            doc.Parse(jsonContent.c_str());

            if (doc.HasParseError()) {
                SKSE::log::error("Erro no parse do JSON em {}", cycleDarJsonPath.string());
                return;
            }

            bool jsonDirty = false;  // Flag para saber se precisamos salvar o JSON

            // --- Bloco 1: Lógica de Conversão BFCO (Sempre checa) ---
            bool shouldConvertToBFCO = false;
            if (doc.HasMember("convertBFCO") && doc["convertBFCO"].IsBool()) {
                shouldConvertToBFCO = doc["convertBFCO"].GetBool();
            }

            if (shouldConvertToBFCO) {
                SKSE::log::info("Iniciando conversão de MCO para BFCO em: {}", cycleDarJsonPath.string());
                int filesRenamed = 0;
                std::filesystem::path destinationPath = cycleDarJsonPath.parent_path();
                for (const auto& fileEntry : std::filesystem::directory_iterator(destinationPath)) {
                    if (fileEntry.is_regular_file()) {
                        std::string filename = fileEntry.path().filename().string();
                        std::string lower_filename = filename;
                        // 2. Converte a cópia para minúsculas.
                        std::transform(lower_filename.begin(), lower_filename.end(), lower_filename.begin(),
                                       [](unsigned char c) { return std::tolower(c); });

                        // CORREÇÃO: Pula arquivos "dodge"
                        if (lower_filename.rfind("mco_", 0) == 0 && lower_filename.find("dodge") == std::string::npos) {
                            std::string newFilename = filename;
                            newFilename.replace(0, 4, "BFCO_");
                            std::filesystem::path newFilePath = destinationPath / newFilename;
                            try {
                                std::filesystem::rename(fileEntry.path(), newFilePath);
                                filesRenamed++;
                            } catch (const std::filesystem::filesystem_error& e) {
                                SKSE::log::error("Falha ao renomear {} para {}. Erro: {}", fileEntry.path().string(),
                                                 newFilePath.string(), e.what());
                            }
                        }
                    }
                }
                SKSE::log::info("Conversão BFCO concluída. {} arquivos renomeados.", filesRenamed);

                // Desliga a flag no JSON para não rodar de novo
                doc["convertBFCO"].SetBool(false);
                jsonDirty = true;
            }

            // --- Bloco 2: Lógica de Cópia (Sources/pathDar) ---
            bool copyDone = false;
            if (doc.HasMember("conversionDone") && doc["conversionDone"].IsBool() && doc["conversionDone"].GetBool()) {
                copyDone = true;
            }

            if (!copyDone) {
                int filesCopied = 0;
                std::filesystem::path destinationPath = cycleDarJsonPath.parent_path();

                // Função auxiliar (igual à original)
                auto processSource = [&](const std::string& relativePath, const rapidjson::Value* filesToCopyArray) {
                    std::filesystem::path sourcePath = "Data" / std::filesystem::path(relativePath);
                    if (!std::filesystem::exists(sourcePath) || !std::filesystem::is_directory(sourcePath)) {
                        SKSE::log::warn("Pasta de origem não existe ou não é um diretório: {}", sourcePath.string());
                        return;
                    }
                    SKSE::log::info("Copiando arquivos de '{}' para '{}'", sourcePath.string(),
                                    destinationPath.string());
                    if (filesToCopyArray && filesToCopyArray->IsArray() && !filesToCopyArray->Empty()) {
                        SKSE::log::info("Modo: Copiando arquivos especificados na lista 'filesToCopy'.");
                        for (const auto& fileValue : filesToCopyArray->GetArray()) {
                            if (fileValue.IsString()) {
                                std::filesystem::path sourceFile = sourcePath / fileValue.GetString();
                                if (std::filesystem::exists(sourceFile)) {
                                    CopySingleFile(sourceFile, destinationPath, filesCopied);
                                } else {
                                    SKSE::log::warn("Arquivo especificado não encontrado na origem: {}",
                                                    sourceFile.string());
                                }
                            }
                        }
                    } else {
                        SKSE::log::info("Modo: Copiando todos os arquivos .hkx da pasta.");
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

                bool copyAttempted = false;
                if (doc.HasMember("sources") && doc["sources"].IsArray()) {
                    copyAttempted = true;
                    for (const auto& sourceObj : doc["sources"].GetArray()) {
                        if (sourceObj.IsObject() && sourceObj.HasMember("path") && sourceObj["path"].IsString()) {
                            const rapidjson::Value* filesArray =
                                sourceObj.HasMember("filesToCopy") ? &sourceObj["filesToCopy"] : nullptr;
                            processSource(sourceObj["path"].GetString(), filesArray);
                        }
                    }
                } else if (doc.HasMember("pathDar") && doc["pathDar"].IsString()) {
                    copyAttempted = true;
                    const rapidjson::Value* filesArray = doc.HasMember("filesToCopy") ? &doc["filesToCopy"] : nullptr;
                    processSource(doc["pathDar"].GetString(), filesArray);
                }

                if (copyAttempted) {
                    SKSE::log::info("Cópia concluída. {} arquivos movidos.", filesCopied);
                    // Marca a CÓPIA como feita
                    if (doc.HasMember("conversionDone")) {
                        doc["conversionDone"].SetBool(true);
                    } else {
                        doc.AddMember("conversionDone", true, doc.GetAllocator());
                    }
                    jsonDirty = true;
                } else if (!shouldConvertToBFCO) {
                    // Se não tentamos copiar E não tentamos converter, então o arquivo é inútil
                    SKSE::log::warn("Formato de CycleDar.json inválido ou não reconhecido em {}",
                                    cycleDarJsonPath.string());
                    return;  // Retorna sem salvar
                }
            } else {
                SKSE::log::info("A cópia para {} já foi concluída anteriormente. Pulando.", cycleDarJsonPath.string());
            }

            // --- Bloco 3: Salvar JSON (se necessário) ---
            if (jsonDirty) {
                rapidjson::StringBuffer buffer;
                rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
                doc.Accept(writer);

                std::ofstream outFile(cycleDarJsonPath);
                if (!outFile) {
                    SKSE::log::error("Falha ao abrir {} para escrita!", cycleDarJsonPath.string());
                    return;
                }

                outFile << buffer.GetString();
                outFile.close();
                SKSE::log::info("Arquivo JSON {} atualizado com sucesso.", cycleDarJsonPath.string());
            }

        } catch (const std::filesystem::filesystem_error& e) {
            SKSE::log::critical("Erro de filesystem em ProcessCycleDarFile para o arquivo '{}': {}",
                                cycleDarJsonPath.string(), e.what());
        }
    }


    // --- DESAFIO 1: Nova função para escanear a pasta do sub-moveset em busca de tags ---
    void ScanSubAnimationFolderForTags(const std::filesystem::path& subAnimPath,
                                                         SubAnimationDef& subAnimDef) {
      try {
        if (!std::filesystem::exists(subAnimPath) || !std::filesystem::is_directory(subAnimPath)) {
            return;
        }

        // --- PONTO 2: Processar CycleDar.json ANTES de escanear as tags ---
        // Procura pelo arquivo CycleDar.json e copia os arquivos .hkx se ele existir.
        // Isso garante que os arquivos copiados estarão presentes para o escaneamento de tags abaixo.
        std::filesystem::path cycleDarPath = subAnimPath / "CycleDar.json";
        if (std::filesystem::exists(cycleDarPath) && std::filesystem::is_regular_file(cycleDarPath)) {
            ProcessCycleDarFile(cycleDarPath);  // Chama a nova função helper
        }


        subAnimDef.attackCount = 0;
        subAnimDef.powerAttackCount = 0;
        subAnimDef.hasIdle = false;
        subAnimDef.hasAnimations = false;
        subAnimDef.dpaTags = {};
        subAnimDef.hasCPA = false;  // Valor inicial
        int hkxFileCount = 0;
        

       // Itera sobre todos os arquivos na pasta para encontrar tags de animação
        for (const auto& fileEntry : std::filesystem::directory_iterator(subAnimPath)) {
            if (fileEntry.is_regular_file()) {
                std::string extension = fileEntry.path().extension().string();
                std::string filename = fileEntry.path().filename().string();
                std::string lowerFilename = filename;
                std::transform(lowerFilename.begin(), lowerFilename.end(), lowerFilename.begin(), ::tolower);
                std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

                if (extension == ".hkx") {
                    hkxFileCount++;

                    // Lógica de contagem de ataques
                    if (lowerFilename.rfind("bfco_attack", 0) == 0) {
                        subAnimDef.attackCount++;
                    }
                    if (lowerFilename.rfind("bfco_powerattack", 0) == 0) {
                        subAnimDef.powerAttackCount++;
                    }
                    if (lowerFilename.find("idle") != std::string::npos) {
                        subAnimDef.hasIdle = true;
                    }

                    // Lógica de verificação de DPA e CPA
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


         logger::info("Scan da pasta '{}': hasDPA (A:{}, B:{}, L:{}, R:{}), hasCPA:{}", subAnimDef.name,
                        subAnimDef.dpaTags.hasA, subAnimDef.dpaTags.hasB, subAnimDef.dpaTags.hasL, subAnimDef.dpaTags.hasR,
                     subAnimDef.hasCPA);
      } catch (const std::filesystem::filesystem_error& e) {
            SKSE::log::critical("Erro de filesystem em ScanSubAnimationFolderForTags na pasta '{}': {}",
                                subAnimPath.string(), e.what());
      }
    }

    // --- Lógica de Escaneamento (Carrega a Biblioteca) ---
    void AnimationManager::ScanAnimationMods() {
        SKSE::log::info("Iniciando escaneamento da biblioteca de animações...");
        try {
        _categories.clear();
        _allMods.clear();

        const std::filesystem::path oarRootPath = "Data\\meshes\\actors\\character\\animations\\OpenAnimationReplacer";
        // ESTRUTURA MELHORADA: Facilita a definição de categorias e suas propriedades
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
            {"Warhammer", 10.0, -1.0, false, false, {}, {}},
            {"Bow", 7.0, -1.0, false, false, {}, {}},
            {"Crossbow", 9.0, -1.0, false, false, {}, {}},
            // Shield
            //{"Shield", -1.0, 11.0, false, true, {}, {}},
            {"Sword & Shield", 1.0, 11.0, false, true, {}, {}},
            {"Dagger & Shield", 2.0, 11.0, false, true, {}, {}},
            {"War Axe & Shield", 3.0, 11.0, false, true, {}, {}},
            {"Mace & Shield", 4.0, 11.0, false, true, {}, {}},
            {"Greatsword & Shield", 5.0, 11.0, false, true, {}, {}},
            {"Battleaxe & Shield", 6.0, 11.0, false, true, {}, {}},
            {"Warhammer & Shield", 10.0, 11.0, false, true, {}, {}},
            // Dual-Wield
            {"Dual Sword", 1.0, 1.0, true, {}, {}},
            {"Dual Greatsword", 5.0, 5.0, true, {}, {}},
            {"Dual Battleaxe", 6.0, 6.0, true, {}, {}},
            {"Dual Warhammer", 10.0, 10.0, true, {}, {}},
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
            _categories[def.name].instances.resize(1);
            _categories[def.name].stanceNames.resize(1);
            _categories[def.name].stanceNameBuffers.resize(1);

            // --- NOVO: Inicializa os nomes e buffers das stances ---
            for (int i = 0; i < _categories[def.name].stanceNames.size(); ++i) {
                std::string defaultName = std::format("Stance {}", i + 1);
                _categories[def.name].stanceNames[i] = defaultName;
                strcpy_s(_categories[def.name].stanceNameBuffers[i].data(),
                         _categories[def.name].stanceNameBuffers[i].size(), defaultName.c_str());
            }
        }
        LoadCustomCategories();
        LoadStances();

        LoadAnimationLibrary();
        SKSE::log::info("Escaneamento de arquivos finalizado. {} mods carregados.", _allMods.size());

        // Agora que temos todos os mods, vamos encontrar quais arquivos já gerenciamos.
        SKSE::log::info("Verificando arquivos previamente gerenciados...");
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
        SKSE::log::info("Encontrados {} arquivos gerenciados.", _managedFiles.size());

        // --- NOVA SEÇÃO: Carregar e integrar movesets do usuário ---
        //LoadUserMovesets();

        for (const auto& userMoveset : _userMovesets) {
            AnimationModDef modDef;
            modDef.name = userMoveset.name;
            modDef.author = "Usuário";  // Autor padrão

            for (const auto& subInstance : userMoveset.subAnimations) {
                // Verifica se os índices são válidos para evitar crashes
                if (subInstance.sourceModIndex < _allMods.size()) {
                    const auto& sourceMod = _allMods[subInstance.sourceModIndex];
                    if (subInstance.sourceSubAnimIndex < sourceMod.subAnimations.size()) {
                        // Adiciona a definição da sub-animação original ao nosso novo mod virtual
                        modDef.subAnimations.push_back(sourceMod.subAnimations[subInstance.sourceSubAnimIndex]);
                    }
                }
            }
            _allMods.push_back(modDef);
        }
        SKSE::log::info("Integração finalizada. Total de {} mods na biblioteca (incluindo de usuário).", _allMods.size());
        // -- -NOVA CHAMADA-- -
        // Agora que a biblioteca de mods (_allMods) está completa, carregamos a configuração da UI.
        _npcCategories = _categories;
        LoadCycleMovesets();
        Load2HHandleSettings();
        
        SKSE::log::info("Categorias de armas para NPCs inicializadas.");
        } catch (const std::exception& e) {  // <--- E ADICIONAR AQUI
            SKSE::log::critical("Um erro fatal ocorreu durante ScanAnimationMods: {}", e.what());
            RE::DebugNotification("Erro crítico ao escanear animações! Verifique os logs.");
        } catch (...) {
            SKSE::log::critical("Um erro fatal e desconhecido ocorreu durante ScanAnimationMods.");
            RE::DebugNotification("Erro crítico e desconhecido ao escanear animações!");
        }
    }
    

    void AnimationManager::LoadGameDataForEffects() {
        _allMagicEffects.clear();
        _allSpells.clear();

        auto dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) return;

        // Carregar Magic Effects (EffectSetting)
        /*for (const auto& mgef : dataHandler->GetFormArray<RE::EffectSetting>()) {
            if (mgef && mgef->GetFullName() && strlen(mgef->GetFullName()) > 0 && mgef->GetFile(0)) {
                _allMagicEffects.push_back({mgef->GetFormID(), clib_util::editorID::get_editorID(mgef),
                                            mgef->GetFullName(), std::string(mgef->GetFile(0)->GetFilename())});
            }
        }
        SKSE::log::info("Carregados {} Magic Effects.", _allMagicEffects.size());*/

        // Carregar Spells (SpellItem)
        for (const auto& spell : dataHandler->GetFormArray<RE::SpellItem>()) {
            // Filtra spells que não são usáveis pelo jogador ou não têm plugin
            if (spell && spell->GetFile(0) &&
                (spell->GetSpellType() != RE::MagicSystem::SpellType::kLeveledSpell) &&  // Ignora leveled spells
                (spell->GetSpellType() != RE::MagicSystem::SpellType::kAbility ||
                 spell->data.flags.any(
                     RE::SpellItem::SpellFlag::kCostOverride)) &&  // Permite abilities com custo (ex: Lesser Powers)
                (spell->GetSpellType() != RE::MagicSystem::SpellType::kDisease))  // Ignora doenças
            {
                std::string finalName;
                const char* fullName = spell->GetFullName();
                std::string editorID = clib_util::editorID::get_editorID(spell);

                if (fullName && strlen(fullName) > 0) {
                    finalName = fullName;
                } else if (!editorID.empty()) {
                    finalName = editorID;
                } else {
                    finalName = ""; // Adiciona com nome vazio
                }

                _allSpells.push_back({spell->GetFormID(), editorID,
                                      finalName, std::string(spell->GetFile(0)->GetFilename())});
            }
        }
        SKSE::log::info("Carregados {} Spells (filtrados).", _allSpells.size());
    }

    void AnimationManager::ProcessTopLevelMod(const std::filesystem::path& modPath) {
        try {
        std::filesystem::path configPath = modPath / "config.json";
        if (!std::filesystem::exists(configPath)) return;
        std::ifstream fileStream(configPath);
        std::string jsonContent((std::istreambuf_iterator<char>(fileStream)), std::istreambuf_iterator<char>());
        fileStream.close();
        rapidjson::Document doc;
        doc.Parse(jsonContent.c_str());
        if (doc.IsObject() && doc.HasMember("isCycleMovesetFallback") && doc["isCycleMovesetFallback"].GetBool()) {
            SKSE::log::info("Ignorando pasta de fallback gerenciada: {}", modPath.string());
            return;
        }
        if (doc.IsObject() && doc.HasMember("name") && doc.HasMember("author")) {
            AnimationModDef modDef;
            modDef.name = doc["name"].GetString();
            modDef.author = doc["author"].GetString();
            for (const auto& subEntry : std::filesystem::directory_iterator(modPath)) {
                if (subEntry.is_directory() && std::filesystem::exists(subEntry.path() / "config.json")) {
                    SubAnimationDef subAnimDef;
                    subAnimDef.name = PathToUTF8(subEntry.path().filename());
                    subAnimDef.path = subEntry.path() / "config.json";
                    ScanSubAnimationFolderForTags(subEntry.path(), subAnimDef);
                    modDef.subAnimations.push_back(subAnimDef);
                }
            }
            _allMods.push_back(modDef);
        }
        } catch (const std::filesystem::filesystem_error& e) {
            SKSE::log::critical("Erro de filesystem em ProcessTopLevelMod ao escanear '{}': {}", modPath.string(),
                                e.what());
            RE::DebugNotification(
                std::format("ERROR scanning mod folder: {}. Check logs.", modPath.filename().string()).c_str());
        }
    }
    void AnimationManager::GenerateFallbackFolders() {
        SKSE::log::info("Iniciando geração/atualização das pastas de fallback...");

        // Caminho base para as nossas pastas de fallback.
        const std::filesystem::path oarRootPath = "Data\\meshes\\actors\\character\\animations\\OpenAnimationReplacer";
        const std::filesystem::path fallbackRootPath = oarRootPath / "_CMF Fallbacks";

        try {
            // Garante que a pasta base "_CycleMoveset_Fallbacks" exista
            if (!std::filesystem::exists(fallbackRootPath)) {
                std::filesystem::create_directories(fallbackRootPath);
            }
        } catch (const std::filesystem::filesystem_error& e) {
            SKSE::log::error("Falha ao criar o diretório raiz de fallback: {}. Erro: {}", fallbackRootPath.string(),
                             e.what());
            return;
        }

        {
            std::filesystem::path rootConfigPath = fallbackRootPath / "config.json";
            rapidjson::Document doc;
            doc.SetObject();
            auto& allocator = doc.GetAllocator();

            // Adiciona informações básicas para que o OAR reconheça a pasta
            doc.AddMember("name", "[CMF] Fallback Animations", allocator);
            doc.AddMember("author", "viny", allocator);
            doc.AddMember("description", "Pasta auto-gerada para conter animações de fallback. Não edite manualmente.",
                          allocator);

            // Adiciona nosso identificador para que ScanAnimationMods ignore esta pasta
            doc.AddMember("isCycleMovesetFallback", true, allocator);

            // Salva o arquivo
            std::ofstream ofs(rootConfigPath);
            if (ofs) {
                rapidjson::StringBuffer buffer;
                rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
                doc.Accept(writer);
                ofs << buffer.GetString();
                ofs.close();
            } else {
                SKSE::log::error("Falha ao criar o config.json raiz para a pasta de fallback!");
            }
        }

        // Processa tanto as categorias do jogador quanto as de todas as regras de NPC
        for (const auto& categoryPair : _categories) {
            const WeaponCategory& category = categoryPair.second;

            // O jogador SEMPRE tem 4 stances (índices 0 a 3)
            for (int i = 0; i < category.instances.size(); ++i) {
                const CategoryInstance& instance = category.instances[i];
                // Se não houver nenhum moveset configurado para esta stance, não há necessidade de criar uma pasta de
                // fallback para ela.
                int playlistCounter = 1;
                for (const auto& modInst : instance.modInstances) {
                    if (!modInst.isSelected) {
                        playlistCounter++;  // Incrementa o contador mesmo se o moveset for pulado
                        continue;
                    }

                    const auto& sourceMod = _allMods[modInst.sourceModIndex];

                    // 3. CRIAÇÃO DA PASTA ÚNICA PARA O MOVESET
                    std::string sanitizedModName = sourceMod.name;
                    std::ranges::replace_if(sanitizedModName, [](char c) { return !isalnum(c); }, '_');

                    std::string fallbackFolderName =
                        std::format("Fallback_{}_{}_P{}_{}", category.name, i, playlistCounter, sanitizedModName);
                    std::filesystem::path fallbackMovesetPath = fallbackRootPath / fallbackFolderName;

                    try {
                        std::filesystem::create_directory(fallbackMovesetPath);
                    } catch (const std::filesystem::filesystem_error& e) {
                        SKSE::log::error("Falha ao criar o diretório de fallback do moveset: {}. Erro: {}",
                                         fallbackMovesetPath.string(), e.what());
                        playlistCounter++;
                        continue;
                    }

                    // 4. LÓGICA DE CÓPIA COM PRIORIDADE PARA O PAI
                    std::set<std::string> copiedFiles;
                    int filesCopiedCount = 0;

                    // Separa os sub-movesets em pais e filhos para dar prioridade
                    std::vector<const SubAnimationInstance*> parentSubInsts;
                    std::vector<const SubAnimationInstance*> childSubInsts;
                    for (const auto& subInst : modInst.subAnimationInstances) {
                        if (!subInst.isSelected) continue;

                        bool isParent = !(subInst.pFront || subInst.pBack || subInst.pLeft || subInst.pRight ||
                                          subInst.pFrontRight || subInst.pFrontLeft || subInst.pBackRight ||
                                          subInst.pBackLeft || subInst.pRandom || subInst.pDodge);

                        if (isParent) {
                            parentSubInsts.push_back(&subInst);
                        } else {
                            childSubInsts.push_back(&subInst);
                        }
                    }

                    // Função auxiliar para copiar os arquivos de um sub-moveset
                    auto copySubMovesetFiles = [&](const SubAnimationInstance* subInst) {
                        const auto& sourceSubAnim =
                            _allMods[subInst->sourceModIndex].subAnimations[subInst->sourceSubAnimIndex];
                        std::filesystem::path sourceDirectory = sourceSubAnim.path.parent_path();

                        if (std::filesystem::exists(sourceDirectory) &&
                            std::filesystem::is_directory(sourceDirectory)) {
                            for (const auto& fileEntry : std::filesystem::directory_iterator(sourceDirectory)) {
                                if (fileEntry.is_regular_file() && fileEntry.path().extension() == ".hkx") {
                                    std::string filename = fileEntry.path().filename().string();
                                    // A condição principal: só copia se o arquivo ainda não foi copiado (dando
                                    // prioridade ao pai)
                                    if (copiedFiles.find(filename) == copiedFiles.end()) {
                                        CopySingleFile(fileEntry.path(), fallbackMovesetPath, filesCopiedCount);
                                        copiedFiles.insert(filename);
                                    }
                                }
                            }
                        }
                    };

                    // Primeiro, copia os arquivos de todos os pais
                    for (const auto* parent : parentSubInsts) {
                        copySubMovesetFiles(parent);
                    }
                    // Depois, copia os arquivos dos filhos (que ainda não existirem)
                    for (const auto* child : childSubInsts) {
                        copySubMovesetFiles(child);
                    }

                    // 5. GERAÇÃO DO CONFIG.JSON PARA O MOVESET
                    if (filesCopiedCount > 0) {
                        rapidjson::Document doc;
                        doc.SetObject();
                        auto& allocator = doc.GetAllocator();

                        doc.AddMember("name", rapidjson::Value(fallbackFolderName.c_str(), allocator), allocator);
                        doc.AddMember("priority", 2095000000, allocator);  // Prioridade intermediária
                        doc.AddMember("isCycleMovesetFallback", true, allocator);

                        rapidjson::Value conditions(rapidjson::kArrayType);
                        rapidjson::Value masterAndBlock(rapidjson::kObjectType);
                        masterAndBlock.AddMember("condition", "AND", allocator);
                        rapidjson::Value andConditions(rapidjson::kArrayType);

                        // Condições: Player, Stance, Arma e o `testarone` específico deste moveset
                        AddIsActorBaseCondition(andConditions, "Skyrim.esm", 0x7, false, allocator);
                        AddCompareValuesCondition(andConditions, "cycle_instance", i + 1, allocator);
                        AddFullCategoryConditions(andConditions, category, allocator);
                        AddCompareValuesCondition(andConditions, "testarone", playlistCounter, allocator);

                        masterAndBlock.AddMember("Conditions", andConditions, allocator);
                        conditions.PushBack(masterAndBlock, allocator);
                        doc.AddMember("conditions", conditions, allocator);

                        std::ofstream ofs(fallbackMovesetPath / "config.json");
                        rapidjson::StringBuffer buffer;
                        rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
                        doc.Accept(writer);
                        ofs << buffer.GetString();

                    } else {
                        std::filesystem::remove(fallbackMovesetPath);
                    }

                    playlistCounter++;  // Incrementa para o próximo moveset na lista
                }  // Fim do loop de movesets
            }  // Fim do loop de stances
        }  // Fim do loop de categorias

        SKSE::log::info("Geração de pastas de fallback por moveset concluída.");
    }

    // --- Lógica da Interface de Usuário ---
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

        // Modal LOC("add_animation") (sem alterações, já estava correto)
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

        // Modal LOC("add_moveset") (COM AS CORREÇÕES)
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
                    bool modNameMatches = filter_str.empty() || mod_name_str.find(filter_str) != std::string::npos;
                    std::vector<size_t> matchingSubAnimIndices;
                    for (size_t subAnimIdx = 0; subAnimIdx < modDef.subAnimations.size(); ++subAnimIdx) {
                        const auto& subAnimDef = modDef.subAnimations[subAnimIdx];
                        std::string sub_anim_name_str = subAnimDef.name;
                        std::transform(sub_anim_name_str.begin(), sub_anim_name_str.end(), sub_anim_name_str.begin(),
                                       ::tolower);
                        if (sub_anim_name_str.find(filter_str) != std::string::npos) {
                            matchingSubAnimIndices.push_back(subAnimIdx);
                        }
                    }
                    if (modNameMatches || !matchingSubAnimIndices.empty()) {
                        if (!filter_str.empty() && mod_name_str.find(filter_str) != std::string::npos) {
                            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
                        }

                        if (ImGui::TreeNode(modDef.name.c_str())) {
                            if (modNameMatches) {
                                // Loop interno pelos submovesets (filhos)
                                for (size_t subAnimIdx = 0; subAnimIdx < modDef.subAnimations.size(); ++subAnimIdx) {
                                    const auto& subAnimDef = modDef.subAnimations[subAnimIdx];

                                    // NENHUM FILTRO AQUI DENTRO. Mostra todos os filhos.

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
                            } else {
                                for (size_t subAnimIdx : matchingSubAnimIndices) {
                                    const auto& subAnimDef = modDef.subAnimations[subAnimIdx];

                                    // NENHUM FILTRO AQUI DENTRO. Mostra todos os filhos.

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
                            }
                            ImGui::TreePop();
                        }
                    }
                }
            }
            ImGui::EndChild();
            if (ImGui::Button(LOC("close"))) {
                strcpy_s(_subMovesetFilter, "");  // Limpa o filtro ao fechar
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    void CreatePerkListJsonFor2H(rapidjson::Document& doc, rapidjson::Value& targetObject, const std::string& keyName,
                                 const std::vector<PerkDef>& perkList) {
        if (perkList.empty()) return;
        rapidjson::Value perkArray(rapidjson::kArrayType);
        for (const auto& perk : perkList) {
            rapidjson::Value perkData(rapidjson::kArrayType);
            perkData.PushBack(rapidjson::Value(perk.pluginName.c_str(), doc.GetAllocator()), doc.GetAllocator());
            perkData.PushBack(perk.formID, doc.GetAllocator());
            // A origem é implícita pelo arquivo, não precisa salvar aqui.
            perkArray.PushBack(perkData, doc.GetAllocator());
        }
        targetObject.AddMember(rapidjson::Value(keyName.c_str(), doc.GetAllocator()), perkArray, doc.GetAllocator());
    }

    // Função auxiliar para carregar (similar a ParsePerkListJson)
    void ParsePerkListJsonFor2H(const rapidjson::Value& sourceObject, const std::string& keyName,
                                std::vector<PerkDef>& targetList) {
        targetList.clear();  // Limpa antes de carregar
        if (!sourceObject.HasMember(keyName.c_str()) || !sourceObject[keyName.c_str()].IsArray()) {
            return;
        }
        const auto& perkArray = sourceObject[keyName.c_str()].GetArray();
        for (const auto& perkData : perkArray) {
            if (perkData.IsArray() && perkData.Size() == 2 && perkData[0].IsString() && perkData[1].IsUint()) {
                std::string plugin = perkData[0].GetString();
                RE::FormID formID = perkData[1].GetUint();
                targetList.push_back({plugin, formID, ""});  // Origem vazia, definida pelo contexto do arquivo
            }
        }
    }

    void AnimationManager::Save2HHandleSettings(bool isPlayer) {
        const std::filesystem::path configDir = "Data/SKSE/Plugins/CycleMovesets";
        std::filesystem::path filePath = configDir / (isPlayer ? "Player2Handle.json" : "NPC2Handle.json");
        const TwoHandHandleConfig& config = isPlayer ? handle::player2HConfig : handle::npc2HConfig;

        SKSE::log::info("Salvando configurações 2H Handle em {}", filePath.string());

        try {
            if (!std::filesystem::exists(configDir)) {
                std::filesystem::create_directories(configDir);
            }
        } catch (const std::filesystem::filesystem_error& e) {
            SKSE::log::error("Falha ao criar diretório {}: {}", configDir.string(), e.what());
            RE::DebugNotification("ERROR: Failed to save 2H Handle settings!");
            return;
        }

        rapidjson::Document doc;
        doc.SetObject();
        auto& allocator = doc.GetAllocator();

        doc.AddMember("MinimumLevel", config.minimumLevel, allocator);
        CreatePerkListJsonFor2H(doc, doc, "RequiredPerks", config.requiredPerks);  // Usa a função auxiliar
        if (!isPlayer) {
            CreatePerkListJsonFor2H(doc, doc, "RequiredPerksDual2H",
                                    config.requiredPerksDual2H);  // <-- Salva o novo campo
        }
        // Salvar o arquivo
        std::ofstream ofs(filePath);
        if (ofs) {
            rapidjson::StringBuffer buffer;
            rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
            doc.Accept(writer);
            ofs << buffer.GetString();
            ofs.close();
            SKSE::log::info("Configurações 2H Handle salvas com sucesso.");
            RE::DebugNotification(isPlayer ? "Player 2H Handle settings saved!" : "NPC 2H Handle settings saved!");
        } else {
            SKSE::log::error("Falha ao abrir {} para escrita!", filePath.string());
            RE::DebugNotification("ERROR: Failed to save 2H Handle settings!");
        }
    }

    void AnimationManager::Load2HHandleSettings() {
        const std::filesystem::path configDir = "Data/SKSE/Plugins/CycleMovesets";
        const std::vector<std::pair<std::string, TwoHandHandleConfig*>> configs = {
            {"Player2Handle.json", &handle::player2HConfig}, {"NPC2Handle.json", &handle::npc2HConfig}};

        SKSE::log::info("Carregando configurações 2H Handle...");

        for (const auto& pair : configs) {
            std::filesystem::path filePath = configDir / pair.first;
            TwoHandHandleConfig* targetConfig = pair.second;

            if (!std::filesystem::exists(filePath)) {
                SKSE::log::info("Arquivo {} não encontrado. Usando valores padrão.", filePath.string());
                *targetConfig = {};  // Reseta para o padrão (nível 0, sem perks)
                continue;
            }

            std::ifstream ifs(filePath);
            if (!ifs) {
                SKSE::log::error("Falha ao abrir {} para leitura!", filePath.string());
                continue;
            }

            std::string jsonContent((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            ifs.close();

            rapidjson::Document doc;
            doc.Parse(jsonContent.c_str());

            if (doc.HasParseError() || !doc.IsObject()) {
                SKSE::log::error("Erro no parse do JSON ou arquivo não é um objeto: {}. Usando padrão.",
                                 filePath.string());
                *targetConfig = {};
                continue;
            }

            if (doc.HasMember("MinimumLevel") && doc["MinimumLevel"].IsInt()) {
                targetConfig->minimumLevel = doc["MinimumLevel"].GetInt();
            } else {
                targetConfig->minimumLevel = 0;
            }

            ParsePerkListJsonFor2H(doc, "RequiredPerks", targetConfig->requiredPerks);  // Usa a função auxiliar
            if (targetConfig == &handle::npc2HConfig) {
                ParsePerkListJsonFor2H(doc, "RequiredPerksDual2H",
                                       targetConfig->requiredPerksDual2H);  // <-- Carrega o novo campo
                SKSE::log::info("... Perks Dual 2H: {}", targetConfig->requiredPerksDual2H.size());  // Log adicional
            } else {
                // Garante que a lista dual do player esteja vazia se carregar de um arquivo antigo
                targetConfig->requiredPerksDual2H.clear();
            }
            SKSE::log::info("Configurações carregadas de {}: Nível {}, {} Perks.", filePath.string(),
                            targetConfig->minimumLevel, targetConfig->requiredPerks.size());
        }
        SKSE::log::info("Carregamento das configurações 2H Handle concluído.");
    }
    // Esta é a nova função principal da UI que você registrará no SKSEMenuFramework
    void AnimationManager::DrawMainMenu() {
        // Primeiro, desenhamos o sistema de abas
        if (ImGui::BeginTabBar("MainTabs")) {
            if (ImGui::BeginTabItem(LOC("tab_movesets"))) {
                DrawAnimationManager();  // Chama a UI da primeira aba
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(LOC("tab_2h_handle"))) {  // Nome da aba traduzido
                // Chama a função reutilizável com os parâmetros do JOGADOR
                Draw2HHandleTabContent(LOC("player_2h_handle_settings_title"),  // Título
                                       handle::player2HConfig,                // Config do jogador
                                       "##Player2HPerks",                       // ID botão perk
                                       "##PlayerLevel",                         // ID input nível
                                       LOC("tooltip_player_2h_perks"),          // Tooltip perk
                                       LOC("tooltip_player_2h_level"),          // Tooltip nível
                                       LOC("save_player_2h_settings"),          // Texto botão salvar
                                       true,                                    // isPlayerConfig = true
                                       false                                    // isNPC = false
                );
                ImGui::EndTabItem();
            }
            //if (ImGui::BeginTabItem(LOC("tab_moveset_creator"))) {
            //    DrawUserMovesetCreator();  // Chama a nova UI
            //    ImGui::EndTabItem();
            //}
            //if (ImGui::BeginTabItem(LOC("tab_user_movesets"))) {
            //    DrawUserMovesetManager();  // Chama a UI da segunda aba
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
        DrawStanceManagementPopup();
        DrawStanceEditorPopup();
        DrawRestartPopup();
        DrawCreateCategoryModal();
        DrawConditionsEffectsPopup();
        DrawHitCountNumberPopup();
        
    }

    // Nova função para desenhar a interface de criação de movesets
    void AnimationManager::DrawUserMovesetCreator() {
        ImGui::Text("Moveset Creator its a tool for moveset Authors");
        ImGui::Separator();

        // Seção de botões principais
        if (ImGui::Button(LOC("save"))) {
            SaveUserMoveset();
        }
        ImGui::SameLine();
        if (ImGui::Button("Read DAR animations")) {
            ScanDarAnimations();
        }
        ImGui::Separator();

        // Campos de Informação do Moveset
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

                                    // --- INÍCIO DA ADIÇÃO #2: BOTÕES DE REORDENAMENTO ---
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
                                    // --- FIM DA ADIÇÃO #2 ---

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

                                    // Seção para gerenciar arquivos .hkx individuais
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
                                // --- INÍCIO DA ADIÇÃO #3: APLICA A MUDANÇA DE ORDEM ---
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

    void AnimationManager::Draw2HHandleTabContent(
        const char* title,                      // Título da seção (Player ou NPC)
        TwoHandHandleConfig& config,  // Referência à config (player ou npc)
        const char* perkButtonID,               // ID único para o botão de perks normal
        const char* levelInputID,               // ID único para o input de nível
        const char* perkTooltip,                // Tooltip do botão de perks normal
        const char* levelTooltip,               // Tooltip do input de nível
        const char* saveButtonText,             // Texto do botão Salvar
        bool isPlayerConfig,                    // Flag para Save2HHandleSettings
        bool isNPC                              // Flag para mostrar campo extra de NPC
    ) {
        ImGui::Text("%s", title);  // Usa o título passado como parâmetro
        ImGui::Separator();
        ImGui::Spacing();

        // --- Perks Normais ---
        std::string perkLabel =
            std::format("{} ({}){}", LOC("required_perks"), config.requiredPerks.size(), perkButtonID);  // Usa LOC e ID
        if (ImGui::Button(perkLabel.c_str())) {
            _perksToDisplayInPopup = config.requiredPerks;
            _inheritedPerkFormIDs.clear();
            _effectsToDisplayInPopup.clear();  // Limpa effects (não aplicável para 2H config)
            _inheritedEffectFormIDs.clear();

            // Define ambos os sets de ponteiros (embora só perks sejam usados aqui)
            _stanceToEditPerk = nullptr;
            _movesetToEditPerk = nullptr;
            _subMovesetToEditPerk = nullptr;
            _stanceToEditEffect = nullptr;
            _movesetToEditEffect = nullptr;
            _subMovesetToEditEffect = nullptr;

            _editingPlayer2HPerks = isPlayerConfig;
            _editingNPC2HPerks = !isPlayerConfig;
            _editingNPCDual2HPerks = false;

            _isConditionsEffectsPopupOpen = true;  // Abre o popup combinado
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", perkTooltip);  // Usa o tooltip passado
        }

        ImGui::Spacing();

        // --- Nível Mínimo ---
        ImGui::PushItemWidth(200);
        // Adiciona o ID único ao InputInt
        if (ImGui::InputInt(std::format("{} {}", LOC("minimum_level"), levelInputID).c_str(), &config.minimumLevel)) {
            if (config.minimumLevel < 0) config.minimumLevel = 0;
        }
        ImGui::PopItemWidth();
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", levelTooltip);  // Usa o tooltip passado
        }

        ImGui::Spacing();

        // --- Perks Dual 2H (SOMENTE NPC) ---
        if (isNPC) {
            ImGui::Separator();  // Separador visual
            ImGui::Spacing();

            std::string dualPerkLabel = std::format("{} ({})##NPCDual2HPerks", LOC("required_perks_dual_2h"),
                                                    config.requiredPerksDual2H.size());  // Label e ID únicos
            if (ImGui::Button(dualPerkLabel.c_str())) {
                _perksToDisplayInPopup = config.requiredPerksDual2H;
                _inheritedPerkFormIDs.clear();
                _effectsToDisplayInPopup.clear();
                _inheritedEffectFormIDs.clear();

                for (const auto& perk : config.requiredPerks) {
                    _perksToDisplayInPopup.push_back(perk);
                    _inheritedPerkFormIDs.insert(perk.formID);
                }
                _perksToDisplayInPopup.insert(_perksToDisplayInPopup.end(), config.requiredPerksDual2H.begin(),
                                              config.requiredPerksDual2H.end());

                // Define ambos os sets de ponteiros
                _stanceToEditPerk = nullptr;
                _movesetToEditPerk = nullptr;
                _subMovesetToEditPerk = nullptr;
                _stanceToEditEffect = nullptr;
                _movesetToEditEffect = nullptr;
                _subMovesetToEditEffect = nullptr;

                _editingPlayer2HPerks = false;
                _editingNPC2HPerks = false;
                _editingNPCDual2HPerks = true;  // Ativa a flag DUAL

                _isConditionsEffectsPopupOpen = true;  // Abre o popup combinado
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", LOC("tooltip_required_perks_dual_2h"));  // Tooltip específico
            }
            ImGui::Spacing();
        }

        ImGui::Separator();
        ImGui::Spacing();

        // Botão Salvar
        if (ImGui::Button(saveButtonText)) {       // Usa o texto passado
            Save2HHandleSettings(isPlayerConfig);  // Usa a flag passada
        }
    }

    void AnimationManager::DrawNPCMenu() { 
        if (ImGui::BeginTabBar("NPCTabs")) {
            if (ImGui::BeginTabItem("NPC Rules")) {
                    DrawNPCManager();
                    ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(LOC("tab_npc_2h_handle"))) {  // Nome da aba traduzido
                // Chama a função reutilizável com os parâmetros do NPC
                Draw2HHandleTabContent(LOC("npc_2h_handle_settings_title"),  // Título
                                       handle::npc2HConfig,                // Config do NPC
                                       "##NPC2HPerks",                       // ID botão perk normal
                                       "##NPCLevel",                         // ID input nível
                                       LOC("tooltip_npc_2h_perks"),          // Tooltip perk normal
                                       LOC("tooltip_npc_2h_level"),          // Tooltip nível
                                       LOC("save"),         
                                       false,                                // isPlayerConfig = false
                                       true  // isNPC = true (para mostrar o campo extra)
                );
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        DrawAddModModal();
        DrawRestartPopup();
        DrawNpcSelectionModal();
        DrawConditionsEffectsPopup();
    }

    int AnimationManager::GetMaxMovesetsFor(const std::string& category, int stanceIndex) { 
        auto it = _maxMovesetsPerCategory.find(category);
        if (it != _maxMovesetsPerCategory.end()) {
            const auto& counts = it->second;
            // CORREÇÃO: Verifica os limites do vetor dinâmico
            if (stanceIndex >= 0 && stanceIndex < counts.size()) {
                return counts[stanceIndex];
            }
        }
        // Se não encontrou a categoria, não há movesets
        return 0;
    }



    const std::map<std::string, WeaponCategory>& AnimationManager::GetCategories() const { 
        return _categories; }

void AnimationManager::DrawCategoryUI(WeaponCategory& category) {
        ImGui::PushID(category.name.c_str());
        if (ImGui::CollapsingHeader(category.name.c_str())) {
            ImGui::BeginGroup();
            if (ImGui::Button("+ Add New Stance")) {
                // Adiciona uma nova stance no final
                category.instances.emplace_back();
                int newStanceNum = category.stanceNames.size() + 1;
                category.stanceNames.push_back(std::format("Stance {}", newStanceNum));

                std::array<char, 64> newBuffer;
                strcpy_s(newBuffer.data(), newBuffer.size(), std::format("Stance {}", newStanceNum).c_str());
                category.stanceNameBuffers.push_back(newBuffer);
            }
            ImGui::Separator();
            std::string tabBarId = std::format("StanceTabs_{}_v{}", category.name, category.uiTabVersion);
            if (ImGui::BeginTabBar(tabBarId.c_str())) {
                // Variável para marcar uma stance para deleção DEPOIS que o loop da tabbar terminar
                

                for (int i = 0; i < category.instances.size(); ++i) {
                    const char* currentStanceName = category.stanceNameBuffers[i].data();
                    bool tab_open = ImGui::BeginTabItem(currentStanceName);


                    if (tab_open) {
                        category.activeInstanceIndex = i;
                        CategoryInstance& instance = category.instances[i];

                        // Mapa para calcular os números da playlist (lógica inalterada)
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

                        // === INÍCIO DAS MUDANÇAS REQUISITADAS ===

                        // 1. Botão "Edit Stance" (NOVO)
                        if (ImGui::Button(LOC("edit_stance"))) {  
                            _isStanceManagementPopupOpen = true;
                            _categoryToEdit = &category;
                            _stanceIndexToEdit = i;
                            // Prepara o buffer de nome para o popup de edição de nome
                            strcpy_s(_editStanceNameBuffer, sizeof(_editStanceNameBuffer), currentStanceName);
                        }

                        // 2. Botão "Add Animation" (Existente, movido para a mesma linha)
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
                            std::string combinedMovesetLabel =
                                std::format("C({}) / E({})##MovesetCondEff_{}", modInstance.perkList.size(),
                                            modInstance.appliedEffects.size(), mod_i);
                            if (ImGui::Button(combinedMovesetLabel.c_str())) {
                                _perksToDisplayInPopup.clear();
                                _inheritedPerkFormIDs.clear();
                                _effectsToDisplayInPopup.clear();
                                _inheritedEffectFormIDs.clear();
                                _inheritedHitRules.clear();              
                                _inheritedHitRules = instance.hitRules;  

                                // Perks: Herda da Stance
                                for (const auto& perk : instance.perkList) {  // 'instance' é a Stance pai
                                    _perksToDisplayInPopup.push_back(perk);
                                    _inheritedPerkFormIDs.insert(perk.formID);
                                }
                                _perksToDisplayInPopup.insert(_perksToDisplayInPopup.end(),
                                                              modInstance.perkList.begin(),
                                                              modInstance.perkList.end());  // Adiciona os próprios

                                // Effects: Herda da Stance
                                for (const auto& effect : instance.appliedEffects) {  // 'instance' é a Stance pai
                                    _effectsToDisplayInPopup.push_back(effect);
                                    _inheritedEffectFormIDs.insert(effect.formID);
                                }
                                _effectsToDisplayInPopup.insert(
                                    _effectsToDisplayInPopup.end(), modInstance.appliedEffects.begin(),
                                    modInstance.appliedEffects.end());  // Adiciona os próprios

                                // Define ponteiros
                                _stanceToEditPerk = nullptr;
                                _movesetToEditPerk = &modInstance;
                                _subMovesetToEditPerk = nullptr;
                                _stanceToEditEffect = nullptr;
                                _movesetToEditEffect = &modInstance;  // Define também o de efeito
                                _subMovesetToEditEffect = nullptr;

                                _editingPlayer2HPerks = false;
                                _editingNPC2HPerks = false;
                                _editingNPCDual2HPerks = false;

                                _isConditionsEffectsPopupOpen = true;
                            }
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

                                    // --- Coluna 1 (Info) ---
                                    ImGui::BeginGroup();
                                    ImGui::Checkbox("##subselect", &subInstance.isSelected);
                                    ImGui::SameLine();

                                    std::string combinedSubLabel =
                                        std::format("C({}) / E({})##SubCondEff_{}_{}",
                                                    subInstance.perkList.size(), subInstance.appliedEffects.size(),
                                                    mod_i,   
                                                    sub_j);  

                                    if (ImGui::Button(combinedSubLabel.c_str())) {
                                        
                                        _perksToDisplayInPopup.clear();
                                        _inheritedPerkFormIDs.clear();
                                        _effectsToDisplayInPopup.clear();
                                        _inheritedEffectFormIDs.clear();
                                        _inheritedHitRules.clear();  
                                        _inheritedHitRules = instance.hitRules;  
                                        _inheritedHitRules.insert(_inheritedHitRules.end(),
                                                                  modInstance.hitRules.begin(),
                                                                  modInstance.hitRules.end());

                                       
                                        for (const auto& perk : instance.perkList) {
                                            _perksToDisplayInPopup.push_back(perk);
                                            _inheritedPerkFormIDs.insert(perk.formID);
                                        }
                                        
                                        for (const auto& perk : modInstance.perkList) {
                                            if (_inheritedPerkFormIDs.find(perk.formID) ==
                                                _inheritedPerkFormIDs.end()) {
                                                _perksToDisplayInPopup.push_back(perk);
                                                _inheritedPerkFormIDs.insert(perk.formID);
                                            }
                                        }
                                        
                                        _perksToDisplayInPopup.insert(_perksToDisplayInPopup.end(),
                                                                      subInstance.perkList.begin(),
                                                                      subInstance.perkList.end());

                                        
                                        for (const auto& effect : instance.appliedEffects) {
                                            _effectsToDisplayInPopup.push_back(effect);
                                            _inheritedEffectFormIDs.insert(effect.formID);
                                        }
                                        
                                        for (const auto& effect : modInstance.appliedEffects) {
                                            if (_inheritedEffectFormIDs.find(effect.formID) ==
                                                _inheritedEffectFormIDs.end()) {
                                                _effectsToDisplayInPopup.push_back(effect);
                                                _inheritedEffectFormIDs.insert(effect.formID);
                                            }
                                        }
                                        
                                        _effectsToDisplayInPopup.insert(_effectsToDisplayInPopup.end(),
                                                                        subInstance.appliedEffects.begin(),
                                                                        subInstance.appliedEffects.end());

                                        
                                        _stanceToEditPerk = nullptr;
                                        _movesetToEditPerk = nullptr;
                                        _subMovesetToEditPerk = &subInstance;
                                        _stanceToEditEffect = nullptr;
                                        _movesetToEditEffect = nullptr;
                                        _subMovesetToEditEffect = &subInstance;  

                                        
                                        _editingPlayer2HPerks = false;
                                        _editingNPC2HPerks = false;
                                        _editingNPCDual2HPerks = false;

                                        
                                        _isConditionsEffectsPopupOpen = true;
                                    }
                                    ImGui::SameLine();
                                    ImGui::BeginGroup();

                                    ImVec2 selectableSize;
                                    ImVec2 contentRegionAvail;
                                    ImGui::GetContentRegionAvail(&contentRegionAvail);
                                    selectableSize.x = contentRegionAvail.x * 0.5f;  // Metade do espaço restante
                                    selectableSize.y = ImGui::GetTextLineHeight();

                                    if (_subInstanceBeingEdited == currentSubInstancePtr) {
                                        ImGui::PushItemWidth(250);
                                        ImGui::SetKeyboardFocusHere();  // Foco automático ao entrar no modo de edição
                                        if (ImGui::InputText("##SubAnimNameEdit", subInstance.editedName.data(),
                                                             subInstance.editedName.size(),
                                                             ImGuiInputTextFlags_EnterReturnsTrue |
                                                                 ImGuiInputTextFlags_AutoSelectAll)) {
                                            _subInstanceBeingEdited =
                                                nullptr;  // Sai do modo de edição ao pressionar Enter
                                        }
                                        // Sai do modo de edição se o campo perder o foco
                                        if (ImGui::IsItemDeactivatedAfterEdit()) {
                                            _subInstanceBeingEdited = nullptr;
                                        }
                                        ImGui::PopItemWidth();

                                    } else {
                                        // Determina qual nome usar: o editado, ou o original se o editado estiver vazio.
                                        const char* displayName = (subInstance.editedName[0] != '\0')
                                                                      ? subInstance.editedName.data()
                                                                      : originSubAnim.name.c_str();

                                        // Constrói a label usando o 'displayName' correto.
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


                                        // Desenha o texto selecionável
                                        ImGui::Selectable(label.c_str(), false, 0,
                                                          ImVec2(250, ImGui::GetTextLineHeight()));

                                        // GATILHO DE EDIÇÃO AGORA É UM MENU DE CONTEXTO (CLIQUE DIREITO)
                                        if (ImGui::BeginPopupContextItem("sub_anim_context_menu")) {
                                            if (ImGui::MenuItem("Edit Name")) {
                                                _subInstanceBeingEdited =
                                                    currentSubInstancePtr;  // Ativa o modo de edição
                                            }
                                            ImGui::EndPopup();
                                        }

                                        // LÓGICA DE DRAG AND DROP (permanece no Selectable)
                                        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                                            ImGui::SetDragDropPayload("DND_SUB_INSTANCE", &sub_j, sizeof(size_t));
                                            ImGui::Text("Move %s", originSubAnim.name.c_str());
                                            ImGui::EndDragDropSource();
                                        }
                                    }

                                    // O target do Drag and Drop pode ficar fora do if/else para funcionar sempre
                                    if (ImGui::BeginDragDropTarget()) {
                                        if (const ImGuiPayload* payload =
                                                ImGui::AcceptDragDropPayload("DND_SUB_INSTANCE")) {
                                            size_t source_idx = *(const size_t*)payload->Data;
                                            std::swap(modInstance.subAnimationInstances[source_idx],
                                                      modInstance.subAnimationInstances[sub_j]);
                                        }
                                        ImGui::EndDragDropTarget();
                                    }

                                    // Tooltip com o nome original (opcional, mas útil)
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

                                    // --- Coluna 2 (Checkboxes) ---
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
            if (_categoryToApplyDeletion == &category && _stanceIndexToDelete != -1) {
                // Checagem de segurança para garantir que o índice ainda é válido
                if (_stanceIndexToDelete < category.instances.size()) {
                    category.instances.erase(category.instances.begin() + _stanceIndexToDelete);
                    category.stanceNames.erase(category.stanceNames.begin() + _stanceIndexToDelete);
                    category.stanceNameBuffers.erase(category.stanceNameBuffers.begin() + _stanceIndexToDelete);

                    // Importante: precisamos reajustar o activeInstanceIndex se ele
                    // for afetado pela deleção!
                    if (category.activeInstanceIndex == _stanceIndexToDelete) {
                        // Se deletamos a aba ativa, voltamos para a primeira
                        category.activeInstanceIndex = 0;
                    } else if (category.activeInstanceIndex > _stanceIndexToDelete) {
                        // Se deletamos uma aba antes da ativa, o índice da ativa diminui
                        category.activeInstanceIndex--;
                    }
                    category.uiTabVersion++;
                }
                // Limpa as flags de deleção
                _categoryToApplyDeletion = nullptr;
                _stanceIndexToDelete = -1;
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
            ImGui::Text("Nenhuma categoria de animação foi carregada.");
            return;
        }

        if(ImGui::BeginTabBar("WeaponTypeTabs")) {
            if (ImGui::BeginTabItem("One-Handed")) {
                for (auto& pair : _categories) {
                    WeaponCategory& category = pair.second;
                    if (category.leftHandEquippedTypeValue != -1.0 && !category.isDualWield &&
                            !category.isShieldCategory) {
                        DrawCategoryUI(category);  // Desenha a UI para esta categoria
                    }
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Two-Handed")) {
                for (auto& pair : _categories) {
                    WeaponCategory& category = pair.second;
                    if (category.leftHandEquippedTypeValue == -1.0) {
                        DrawCategoryUI(category);
                    }
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Dual-Wield")) {
                for (auto& pair : _categories) {
                    WeaponCategory& category = pair.second;
                    if (category.isDualWield) {  // Filtro
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
        // --- LÓGICA DE TROCA DE MODO (LISTA vs. EDIÇÃO) ---
        if (_ruleToEdit != nullptr) {
            // --- MODO DE EDIÇÃO: Mostra o editor para a regra selecionada ---

            if (ImGui::Button("Back")) {
                SKSE::log::info("[DrawNPCManager] Botão 'Voltar' clicado. Saindo do modo de edição.");
                _ruleToEdit = nullptr;  // Define como nulo para voltar ao modo de lista
                return;
            }
            ImGui::SameLine();
            ImGui::TextDisabled(" | Editing rule: ");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s", _ruleToEdit->displayName.c_str());
            ImGui::Separator();

            // Pega o mapa de categorias da regra que estamos editando
            auto& categoriesToDraw = _ruleToEdit->categories;

            if (categoriesToDraw.empty()) {
                ImGui::Text("This rule doesnt have categories");
            }

            // Reutiliza a mesma TabBar e lógica de UI que você já tinha para NPCs
            if (ImGui::BeginTabBar("WeaponTypeTabs_NPC_Edit")) {
                if (ImGui::BeginTabItem("One-Handed")) {
                    for (auto& pair : categoriesToDraw) {
                        WeaponCategory& category = pair.second;
                        if (category.leftHandEquippedTypeValue != -1.0 && !category.isDualWield &&
                            !category.isShieldCategory) {
                            DrawNPCCategoryUI(category);  // Desenha a UI para esta categoria
                        }
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Two-Handed")) {
                    for (auto& pair : categoriesToDraw) {
                        WeaponCategory& category = pair.second;
                        if (category.leftHandEquippedTypeValue == -1.0) {
                            DrawNPCCategoryUI(category);
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
            // --- MODO DE LISTA: Mostra os filtros e a lista de regras (código do passo anterior) ---
            //SKSE::log::info("[DrawNPCManager] Renderizando em MODO DE LISTA.");
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
                // --- Linha Fixa para a Regra "NPCs (General)" ---
                ImGui::TableNextRow();

                // Coluna: Tipo
                ImGui::TableNextColumn();
                ImGui::TextDisabled("General");

                // Coluna: Nome
                ImGui::TableNextColumn();
                ImGui::Text("NPCs (General)");
                ImGui::TextDisabled("Regra base para todos os NPCs que não correspondem a uma regra mais específica.");

                // Coluna: Plugin
                ImGui::TableNextColumn();
                ImGui::Text("Plugin");

                // Coluna: Ações
                ImGui::TableNextColumn();
                ImGui::PushID("##GeneralRule");
                if (ImGui::Button("Edit")) {
                    SKSE::log::info("[DrawNPCManager] Botão 'Editar' da Regra Geral clicado.");
                    _ruleToEdit = &_generalNpcRule;  // Aponta para a nossa variável de membro
                }
                // Sem botão de excluir aqui
                ImGui::PopID();
                std::string filterTextLower = _ruleFilterText;
                std::transform(filterTextLower.begin(), filterTextLower.end(), filterTextLower.begin(), ::tolower);

                for (auto it = _npcRules.begin(); it != _npcRules.end();) {
                    auto& rule = *it;
                    //SKSE::log::info("[DrawNPCManager] Processando regra na lista: '{}'", rule.displayName); 
                    bool skipRule = false;
                    if (_ruleFilterType != 0) {  // Se não for "All"
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
                                // Caso algum outro valor apareça, não filtra nada
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
                    const char* typeName = "Desconhecido";
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
                        SKSE::log::info("[DrawNPCManager] Botão 'Editar' clicado para a regra: '{}'", rule.displayName);
                        _ruleToEdit = &rule;  // <-- A MÁGICA ACONTECE AQUI!
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
        // --- NOVO POP-UP: Adicione este bloco de código no final da função ---
        if (ImGui::BeginPopupModal("Select Rule Type", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Which rule type you want create?");
            ImGui::Separator();

            // Radio buttons para selecionar o tipo de regra
            // A variável _ruleTypeToCreate é a que adicionamos ao .h anteriormente
            ImGui::RadioButton("NPC", reinterpret_cast<int*>(&_ruleTypeToCreate), (int)RuleType::UniqueNPC);
            ImGui::RadioButton("Keyword", reinterpret_cast<int*>(&_ruleTypeToCreate), (int)RuleType::Keyword);
            ImGui::RadioButton("Faction", reinterpret_cast<int*>(&_ruleTypeToCreate), (int)RuleType::Faction);
            ImGui::RadioButton("Race", reinterpret_cast<int*>(&_ruleTypeToCreate), (int)RuleType::Race);

            ImGui::Separator();

            if (ImGui::Button("Next", ImVec2(120, 0))) {
                // Quando o usuário clica em Próximo, nós ativamos o pop-up de seleção principal
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

    // Helper para a UI de Categoria do NPC
void AnimationManager::DrawNPCCategoryUI(WeaponCategory& category) {
        ImGui::PushID(category.name.c_str());
        if (ImGui::CollapsingHeader(category.name.c_str())) {
            // NPCs usam a instância 0 (Stance 0)
            CategoryInstance& instance = category.instances[0];

            // --- PONTO 2: Lógica para calcular a ordem dos movesets ---
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
            // --- FIM DA LÓGICA DE ORDEM ---

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

                ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() * 0.57f);  // Coluna 1 usa 60% do espaço

                // --- COLUNA 1: Controles e Nome do Moveset ---
                if (ImGui::Button("X")) modInstanceToRemove = static_cast<int>(mod_i);
                ImGui::SameLine();
                ImGui::Checkbox("##modselect", &modInstance.isSelected);
                ImGui::SameLine();

                // O TreeNode agora está dentro de uma coluna, então sua largura é limitada.
                bool node_open = ImGui::TreeNode(sourceMod.name.c_str());

                // A lógica de Drag & Drop agora se aplica ao TreeNode, mas a área é limitada pela coluna.
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

                ImGui::NextColumn();  // Passa para a próxima coluna

                if (_instanceBeingEdited == &modInstance) {
                    // MODO DE EDIÇÃO: Mostra os campos de Input
                    ImGui::PushItemWidth(60);  // Define uma largura pequena para cada campo
                    ImGui::InputInt("Hp", &modInstance.hp, 0);
                    ImGui::SameLine();
                    ImGui::InputInt("St", &modInstance.st, 0);
                    ImGui::SameLine();
                    ImGui::InputInt("Mn", &modInstance.mn, 0);
                    ImGui::SameLine();
                    ImGui::InputInt("Lv", &modInstance.level, 0);
                    ImGui::SameLine();
                    ImGui::PopItemWidth();

                    // Botão para salvar e sair do modo de edição
                    if (ImGui::Button("OK")) {
                        // Validação opcional dos dados antes de salvar
                        modInstance.hp = std::clamp(modInstance.hp, 0, 100);
                        modInstance.st = std::clamp(modInstance.st, 0, 100);
                        modInstance.mn = std::clamp(modInstance.mn, 0, 100);
                        if (modInstance.level < 0) modInstance.level = 0;

                        _instanceBeingEdited = nullptr;  // Sai do modo de edição
                    }

                } else {
                    // MODO DE VISUALIZAÇÃO: Mostra o texto
                    std::string conditions_text =
                        std::format("Hp <= {}% | St <= {}% | Mn <= {}% | Lv => {}", modInstance.hp, modInstance.st,
                                    modInstance.mn, modInstance.level);

                    ImGui::Selectable(conditions_text.c_str(), false, 0, ImVec2(0, ImGui::GetTextLineHeight()));

                    // O menu de contexto ATIVA o modo de edição
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
                        _isAddModModalOpen = true;           // Abre o menu modal
                        _modInstanceToAddTo = &modInstance;  // Aponta para o moveset de NPC atual
                        _instanceToAddTo = nullptr;          // Limpa o ponteiro de instância geral
                        _userMovesetToAddTo = nullptr;       // Limpa o ponteiro de moveset do usuário
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

                        // Label que será a área de arrastar
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
                            label = originSubAnim.name;  // Mostra nome simples se desmarcado
                        }

                        // PONTO-CHAVE: Criamos um Selectable com tamanho definido.
                        // Isso restringe a área de arrastar e soltar, deixando espaço para outros widgets.
                        ImVec2 contentRegionAvail;
                        ImGui::GetContentRegionAvail(&contentRegionAvail);
                        ImVec2 selectableSize(contentRegionAvail.x * 0.7f,
                                              ImGui::GetTextLineHeight());  // Ocupa 70% do espaço restante

                        ImGui::Selectable(label.c_str(), false, 0, selectableSize);

                        // Aplicamos o Drag & Drop APENAS ao Selectable que acabamos de criar.
                        if (ImGui::BeginDragDropSource()) {
                            ImGui::SetDragDropPayload("DND_SUB_INSTANCE_NPC", &sub_j, sizeof(size_t));
                            ImGui::Text("Mover %s", originSubAnim.name.c_str());
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

                        ImGui::EndGroup();  // Fim da Coluna 1

                        // --- Coluna 2: Checkboxes de propriedades ---
                        ImGui::SameLine();  // Move o cursor para a mesma linha, ao lado da Coluna 1

                        ImGui::BeginGroup();  // Agrupa os checkboxes para garantir o alinhamento

                        // Seus checkboxes agora estão fora da área de arrastar e são clicáveis.
                        // ImGui::Checkbox("Rnd", &subInstance.pRandom);
                        // ImGui::SameLine(); // Se tiver mais de um, use SameLine
                        ImGui::Checkbox("Movement", &subInstance.pDodge);

                        ImGui::EndGroup();  // Fim da Coluna 2
                        // --- FIM DAS CHECKBOXES ---

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
        SKSE::log::info("Iniciando salvamento global de todas as configurações...");
        SaveCustomCategories();
        SaveStances();
        //SaveStanceNames();
        SaveCycleMovesets();  // Esta já foi corrigida e está funcionando.
        GenerateFallbackFolders();
        SKSE::log::info("Gerando arquivos de condição para OAR...");
        std::map<std::filesystem::path, std::vector<FileSaveConfig>> fileUpdates;

        auto processCategoriesForOAR = [&](const std::map<std::string, WeaponCategory>& sourceCategories,
                                           const MovesetRule* rule = nullptr) {
            bool isNpcRule = (rule != nullptr);  // Determina se é uma regra de NPC ou o Player

            for (const auto& pair : sourceCategories) {
                const WeaponCategory& category = pair.second;

                // --- LÓGICA CORRIGIDA: Determina quantas stances iterar ---
                int maxStances = isNpcRule ? 1 : category.instances.size();  

                for (int i = 0; i < maxStances; ++i) {
                    const CategoryInstance& instance = category.instances[i];

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
                                if (tempLastParentOrder > 0) {  // Garante que há um pai para associar
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
                            std::vector<PerkDef> consolidatedPerks;

                            // Preenche a lista consolidada com perks da hierarquia
                            consolidatedPerks.insert(consolidatedPerks.end(), subInst.perkList.begin(),
                                                     subInst.perkList.end());
                            consolidatedPerks.insert(consolidatedPerks.end(), modInst.perkList.begin(),
                                                     modInst.perkList.end());
                            consolidatedPerks.insert(consolidatedPerks.end(), instance.perkList.begin(),
                                                     instance.perkList.end());

                            // Remove duplicatas baseadas no FormID
                            std::set<RE::FormID> seenPerks;
                            consolidatedPerks.erase(std::remove_if(consolidatedPerks.begin(), consolidatedPerks.end(),
                                                                   [&](const PerkDef& perk) {
                                                                       if (seenPerks.count(perk.formID)) {
                                                                           return true;
                                                                       }
                                                                       seenPerks.insert(perk.formID);
                                                                       return false;
                                                                   }),
                                                    consolidatedPerks.end());

                            // ========================= INÍCIO DA CORREÇÃO =========================
                            // Atribui a lista de structs diretamente. Esta parte já está correta,
                            // mas confirmamos que o tipo de 'config.perkList' é std::vector<PerkDef>.
                            config.perkList = consolidatedPerks; 
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

                                // ETAPA 2: POPULAR O CAMPO childDirections USANDO O MAPA
                                auto it = childDirectionsByParentOrder.find(config.order_in_playlist);
                                if (it != childDirectionsByParentOrder.end()) {
                                    config.childDirections = it->second;
                                }
                                // ======================= FIM DA CORREÇÃO =======================

                            } else {
                                config.order_in_playlist = lastParentOrder;
                            }
                            std::filesystem::path configPath;
                            if (sourceMod.name == "[DAR] Animations") {
                                configPath = sourceSubAnim.path / "user.json";
                            } else {
                                configPath = sourceSubAnim.path.parent_path() / "user.json";
                            }
                            fileUpdates[configPath].push_back(config);
                        }
                    }
                }
            }
        };

        // 2. Coleta as configurações de todas as fontes de regras
        SKSE::log::info("Coletando configurações do Player...");
        processCategoriesForOAR(_categories);
        SKSE::log::info("Coletando configurações de NPCs Gerais...");
        processCategoriesForOAR(_generalNpcRule.categories, &_generalNpcRule);
        SKSE::log::info("Coletando configurações de {} regras específicas...", _npcRules.size());
        for (const auto& specificRule : _npcRules) {
            processCategoriesForOAR(specificRule.categories, &specificRule);
        }

        // 3. Limpa arquivos gerenciados que não estão mais em uso
        for (const auto& managedPath : _managedFiles) {
            if (fileUpdates.find(managedPath) == fileUpdates.end()) {
                fileUpdates[managedPath] = {};  // Adiciona para a fila de desativação
            }
        }
        for (const auto& pair : fileUpdates) {
            _managedFiles.insert(pair.first);
        }

        // 4. Escreve os arquivos config.json usando a lógica atualizada de UpdateOrCreateJson
        SKSE::log::info("{} arquivos de configuração OAR serão modificados.", fileUpdates.size());
        for (const auto& updateEntry : fileUpdates) {
            UpdateOrCreateJson(updateEntry.first, updateEntry.second);
        }

        SKSE::log::info("Salvamento global concluído.");
        RE::DebugNotification("Todas as configurações foram salvas!");
        UpdateMaxMovesetCache();
        if (gameisloaded) {
            _showRestartPopup = true;
        }
        gameisloaded = true;

}

void AnimationManager::DeleteManagedUserJsonFiles() {
    SKSE::log::info("Iniciando a exclusão de todos os arquivos user.json gerenciados...");
    int filesDeleted = 0;

    const std::vector<std::filesystem::path> rootPaths = {
        "Data/meshes/actors/character/animations/OpenAnimationReplacer",
        "Data/meshes/actors/character/animations/DynamicAnimationReplacer/_CustomConditions"};

    auto scanAndCleanDirectory = [&](const std::filesystem::path& root) {
        if (!std::filesystem::exists(root) || !std::filesystem::is_directory(root)) {
            SKSE::log::warn("Diretório de varredura não encontrado: {}", root.string());
            return;
        }

        try {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
                if (entry.is_directory()) {
                    const auto& dirPath = entry.path();

                    // Verifica se a pasta é gerenciada (contém um dos arquivos marcadores)
                    bool isManaged = std::filesystem::exists(dirPath / "CycleMoveset.json") ||
                                     std::filesystem::exists(dirPath / "User_CycleMoveset.json");

                    if (isManaged) {
                        // Lista de todos os arquivos a serem deletados se a pasta for gerenciada
                        const std::vector<std::filesystem::path> filesToDelete = {
                            dirPath / "user.json", dirPath / "CycleMoveset.json", dirPath / "User_CycleMoveset.json"};

                        for (const auto& filePath : filesToDelete) {
                            if (std::filesystem::exists(filePath)) {
                                try {
                                    if (std::filesystem::remove(filePath)) {
                                        SKSE::log::info("Arquivo deletado com sucesso: {}", filePath.string());
                                        filesDeleted++;
                                    } else {
                                        SKSE::log::error("Falha ao deletar o arquivo (motivo desconhecido): {}",
                                                         filePath.string());
                                    }
                                } catch (const std::filesystem::filesystem_error& e) {
                                    SKSE::log::error("Exceção ao tentar deletar o arquivo {}: {}", filePath.string(),
                                                     e.what());
                                }
                            }
                        }
                    }
                }
            }
        } catch (const std::filesystem::filesystem_error& e) {
            SKSE::log::error("Erro de filesystem durante a varredura do diretório {}: {}", root.string(), e.what());
        }
    };

    for (const auto& rootPath : rootPaths) {
        scanAndCleanDirectory(rootPath);
    }

    SKSE::log::info("Exclusão concluída. Total de {} arquivos de configuração deletados.", filesDeleted);
    RE::DebugNotification(std::format("{} configuration files were deleted.", filesDeleted).c_str());
}

void AnimationManager::PopulatePerkList() {
    _allPerks.clear();
    auto dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) return;

    for (const auto& perk : dataHandler->GetFormArray<RE::BGSPerk>()) {
        if (perk && perk->GetFile(0)) {
            std::string finalName;
            const char* fullName = perk->GetFullName();
            std::string editorID = clib_util::editorID::get_editorID(perk);

            if (fullName && strlen(fullName) > 0) {
                finalName = fullName;
            } else if (!editorID.empty()) {
                finalName = editorID;
            } else {
                finalName = "";  // Adiciona com nome vazio
            }

            _allPerks.push_back({perk->GetFormID(), editorID, finalName, std::string(perk->GetFile(0)->GetFilename())});
        }
    }
    SKSE::log::info("Carregados {} perks.", _allPerks.size());
}

bool AnimationManager::CheckActorHasPerks(RE::Actor* actor, const std::vector<PerkDef>& perks) { 
if (!actor) return false;        // Segurança
    if (perks.empty()) return true;  // Se não há perks, está disponível

    for (const auto& perkDef : perks) {
        auto* perkForm = RE::TESForm::LookupByID<RE::BGSPerk>(perkDef.formID);
        if (!perkForm || !actor->HasPerk(perkForm)) {
            // O ator não tem um dos perks necessários
            return false;
        }
    }
    // O ator tem todos os perks
    return true;
}

std::vector<AvailableItem> AnimationManager::GetAvailableStances(RE::Actor* actor, const std::string& categoryName) {
    std::vector<AvailableItem> availableStances;
    auto cat_it = _categories.find(categoryName);
    if (cat_it == _categories.end()) return availableStances;

    WeaponCategory& category = cat_it->second;

    // Itera por todas as stances configuradas (índice 0-based)
    for (int i = 0; i < category.instances.size(); ++i) {
        const auto& instance = category.instances[i];  // Esta é a stance

        // 1. O ator tem os perks para ESTA STANCE?
        if (!CheckActorHasPerks(actor, instance.perkList)) {
            continue;  // Pula esta stance, o jogador não tem o perk
        }

        // 2. Esta stance está VAZIA?
        // (Verifica se ela tem pelo menos UM moveset que também seja válido)
        bool hasAvailableMoveset = false;
        for (const auto& modInst : instance.modInstances) {
            if (!modInst.isSelected || !CheckActorHasPerks(actor, modInst.perkList)) continue;

            for (const auto& subInst : modInst.subAnimationInstances) {
                if (!subInst.isSelected) continue;

                // É um moveset "pai" (não direcional)?
                bool isParent = !(subInst.pFront || subInst.pBack || subInst.pLeft || subInst.pRight ||
                                  subInst.pFrontRight || subInst.pFrontLeft || subInst.pBackRight ||
                                  subInst.pBackLeft || subInst.pRandom || subInst.pDodge);

                // O ator tem os perks para este SUB-MOVESET?
                if (isParent && CheckActorHasPerks(actor, subInst.perkList)) {
                    hasAvailableMoveset = true;
                    break;
                }
            }
            if (hasAvailableMoveset) break;
        }

        // 3. Se a stance passou nos perks E não está vazia, adicione-a à lista
        if (hasAvailableMoveset) {
            availableStances.push_back({category.stanceNames[i], i + 1});  // Salva o nome e o índice 1-based
        }
    }
    return availableStances;
}

std::vector<AvailableItem> AnimationManager::GetAvailableMovesets(RE::Actor* actor, const std::string& categoryName,
                                                                  int stanceOriginalIndex) {
    std::vector<AvailableItem> availableMovesets;
    if (stanceOriginalIndex <= 0) return availableMovesets;

    auto cat_it = _categories.find(categoryName);
    if (cat_it == _categories.end()) return availableMovesets;

    WeaponCategory& category = cat_it->second;
    if (stanceOriginalIndex > category.instances.size()) return availableMovesets;

    const auto& instance = category.instances[stanceOriginalIndex - 1];  // A stance que queremos

    // 1. (Opcional, mas bom) Verificar os perks da stance pai.
    // (A GetAvailableStances já deve ter feito isso, mas é uma boa segurança)
    if (!CheckActorHasPerks(actor, instance.perkList)) {
        return availableMovesets;  // A stance inteira está bloqueada
    }

    int parentCounter = 0;  // Este será o "índice original" do moveset
    for (const auto& modInst : instance.modInstances) {
        if (!modInst.isSelected) continue;

        // 2. O ator tem os perks para este MOVESET (ModInstance)?
        if (!CheckActorHasPerks(actor, modInst.perkList)) {
            // Se o moveset (modpack) está bloqueado, pulamos todos os seus filhos
            // Mas ainda precisamos contar os "pais" dentro dele para manter os índices corretos
            for (const auto& subInst : modInst.subAnimationInstances) {
                bool isParent = !(subInst.pFront || subInst.pBack || subInst.pLeft || subInst.pRight ||
                                  subInst.pFrontRight || subInst.pFrontLeft || subInst.pBackRight ||
                                  subInst.pBackLeft || subInst.pRandom || subInst.pDodge);
                if (isParent) parentCounter++;
            }
            continue;
        }

        // 3. Itera pelos sub-movesets
        for (const auto& subInst : modInst.subAnimationInstances) {
            if (!subInst.isSelected) continue;

            bool isParent =
                !(subInst.pFront || subInst.pBack || subInst.pLeft || subInst.pRight || subInst.pFrontRight ||
                  subInst.pFrontLeft || subInst.pBackRight || subInst.pBackLeft || subInst.pRandom || subInst.pDodge);
            if (!isParent) continue;  // Só nos importamos com os "pais" para a ciclagem

            parentCounter++;  // Achamos um "pai", seu índice é este.

            // 4. O ator tem os perks para este SUB-MOVESET?
            if (CheckActorHasPerks(actor, subInst.perkList)) {
                // SUCESSO! Este moveset está disponível
                const auto& sourceSubAnim = _allMods[subInst.sourceModIndex].subAnimations[subInst.sourceSubAnimIndex];
                const char* displayName =
                    (subInst.editedName[0] != '\0') ? subInst.editedName.data() : sourceSubAnim.name.c_str();

                availableMovesets.push_back({displayName, parentCounter});
            }
        }
    }
    return availableMovesets;
}


    void AnimationManager::UpdateOrCreateJson(const std::filesystem::path& jsonPath,
                                              const std::vector<FileSaveConfig>& configs) {
        rapidjson::Document doc;
        std::ifstream fileStream(jsonPath);
        if (fileStream) {
            std::string jsonContent((std::istreambuf_iterator<char>(fileStream)), std::istreambuf_iterator<char>());
            fileStream.close();
            if (doc.Parse(jsonContent.c_str()).HasParseError()) {
                SKSE::log::error("Erro de Parse ao ler {}. Criando um novo arquivo.", jsonPath.string());
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
                if (!config.perkList.empty()) {
                    for (const auto& perk : config.perkList) {
                        // Agora 'perk' é a nossa struct. Acessamos os membros diretamente.
                        AddHasPerkCondition(andConditions, perk.pluginName, perk.formID, allocator);
                    }
                }
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
                AddFullCategoryConditions(andConditions, *config.category, allocator);
                if (config.category->isDualWield) {
                    // Dual Wield e Escudo exigem que AMBOS os slots de 1H estejam ocupados
                    AddIsEquipSlotOccupiedCondition(andConditions, "RightHand", false, allocator);
                    AddIsEquipSlotOccupiedCondition(andConditions, "LeftHand", false, allocator);
                } else if (config.category->leftHandEquippedTypeValue == -1.0) {
                    // Categoria de Duas Mãos Padrão (ex: Greatsword)
                    AddIsEquipSlotOccupiedCondition(andConditions, "TwoHand", false, allocator);
                } else if (config.category->leftHandEquippedTypeValue == 0.0) {
                    // Categoria de Uma Mão Pura (mão esquerda vazia)
                    AddIsEquipSlotOccupiedCondition(andConditions, "RightHand", false, allocator);
                    AddIsEquipSlotOccupiedCondition(andConditions, "LeftHand", true, allocator);
                }

                // ADIÇÃO: Correção de segurança para a instância do jogador
                int final_instance_index = config.instance_index;
                // Se a configuração é para o jogador (!isNPC) e o índice for inválido (< 1), corrige para 1.
                if (config.ruleType == RuleType::Player && final_instance_index < 1) {
                    SKSE::log::warn(
                        "Índice de instância inválido (0) encontrado para o Jogador em {}. Corrigindo para 1.",
                        jsonPath.string());
                    final_instance_index = 1;  // Garante que o valor mínimo para o jogador seja 1
                    
                }

                if (config.ruleType != RuleType::Player) {
                    final_instance_index = 0;
                }
                AddCompareValuesCondition(andConditions, "cycle_instance", final_instance_index, allocator);
                // Stance and Playlist order
                
                if (config.order_in_playlist > 0) {
                    AddCompareValuesCondition(andConditions, "testarone", config.order_in_playlist, allocator);
                    if (config.isParent) {
                        // Acessa o novo membro diretamente do objeto config!
                        const auto& childDirs = config.childDirections;
                        if (!childDirs.empty()) {

                            // 1. Cria um novo bloco AND para agrupar as condições negadas
                            rapidjson::Value negatedAndBlock(rapidjson::kObjectType);
                            negatedAndBlock.AddMember("condition", "AND", allocator);
                            negatedAndBlock.AddMember("comment", "Is NOT any child direction", allocator);

                            // 2. Cria um array para as condições dentro deste novo bloco
                            rapidjson::Value innerNegatedConditions(rapidjson::kArrayType);

                            // 3. Adiciona todas as condições negadas a ESTE NOVO ARRAY
                            for (int dirValue : childDirs) {
                                AddNegatedCompareValuesCondition(innerNegatedConditions, "DirecionalCycleMoveset",
                                                                 dirValue, allocator);
                            }

                            // 4. Associa o array de condições ao novo bloco AND
                            negatedAndBlock.AddMember("Conditions", innerNegatedConditions, allocator);

                            // 5. Adiciona o bloco AND (que contém todas as negações) ao array principal
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
            SKSE::log::error("Falha ao abrir o arquivo para escrita: {}", jsonPath.string());
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

    // NOVA FUNÇÃO HELPER: Adiciona uma condição "CompareValues" para um valor booleano.
    // Usada para verificar as checkboxes de movimento (F, B, L, R, etc.).
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
        valueB.AddMember("graphVariableType", "bool", allocator);  // O tipo aqui é "bool"
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

    // Toda a parte de user ta ca pra baixo

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
        SKSE::log::info("Atualizando cache de contagem máxima de movesets...");
        _maxMovesetsPerCategory.clear();
        _maxMovesetsPerCategory_NPC.clear();

        // 1. Cache do JOGADOR (lógica inalterada)
        for (auto& pair : _categories) {
            WeaponCategory& category = pair.second;
            std::vector<int> counts(category.instances.size(), 0);
            for (int i = 0; i < category.instances.size(); ++i) {
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
        SKSE::log::info("Cache do Jogador atualizado.");

        // 2. Cache dos NPCS GERAIS (usando FormID 0 como chave)
        for (auto& pair : _npcCategories) {
            WeaponCategory& category = pair.second;
            CategoryInstance& instance = category.instances[0];  // Stance 0 para NPCs
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
        SKSE::log::info("Cache de NPCs Gerais (ID 0) atualizado.");

        // 3. Cache dos NPCS ESPECÍFICOS (usando seu FormID real como chave)
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
            SKSE::log::info("Cache para NPC Específico {:08X} atualizado.", npcFormID);
        }

        SKSE::log::info("Cache de contagem máxima de movesets (Player & Todos NPCs) foi atualizado.");
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

    // Função auxiliar para criar e adicionar o bloco de condição complexo
    //void AnimationManager::AddOcfWeaponExclusionConditions(rapidjson::Value& parentArray,
    //                                                       rapidjson::Document::AllocatorType& allocator) {
    //    rapidjson::Value mainAndBlock(rapidjson::kObjectType);
    //    mainAndBlock.AddMember("condition", "AND", allocator);
    //    mainAndBlock.AddMember("requiredVersion", "1.0.0.0", allocator);

    //    rapidjson::Value innerConditions(rapidjson::kArrayType);

    //    // Lista dos editorIDs das armas a serem excluídas
    //    const std::vector<const char*> keywords = {
    //        "OCF_WeapTypeRapier1H", "OCF_WeapTypeRapier2H",    "OCF_WeapTypeKatana1H",   "OCF_WeapTypeKatana2H",
    //        "OCF_WeapTypePike1H",   "OCF_WeapTypePike2H",      "OCF_WeapTypeHalberd2H",  "OCF_WeapTypeHalberd1H",
    //        "OCF_WeapTypeClaw1H",   "OCF_WeapTypeTwinblade1H", "OCF_WeapTypeTwinblade2H"};

    //    // Função lambda para criar uma condição "IsEquippedHasKeyword" negada
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

    //    // Adiciona as condições para a maioria das armas (mão direita e esquerda)
    //    for (const auto& keyword : keywords) {
    //        innerConditions.PushBack(createNegatedKeywordCondition(keyword, false), allocator);  // Left hand: false
    //        innerConditions.PushBack(createNegatedKeywordCondition(keyword, true), allocator);   // Left hand: true
    //    }

    //    // Casos especiais do Quarterstaff que não seguem o padrão par
    //    innerConditions.PushBack(createNegatedKeywordCondition("OCF_WeapTypeQuarterstaff2H", false), allocator);
    //    innerConditions.PushBack(createNegatedKeywordCondition("OCF_WeapTypeQuarterstaff1H", true), allocator);

    //    mainAndBlock.AddMember("Conditions", innerConditions, allocator);
    //    parentArray.PushBack(mainAndBlock, allocator);
    //}

void AnimationManager::AddCompetingKeywordExclusions(rapidjson::Value& parentArray,
                                                         const WeaponCategory* currentCategory, bool isLeftHand,
                                                         rapidjson::Document::AllocatorType& allocator) {
        // --- CORREÇÃO: Usar um std::set para garantir que cada keyword seja única ---
        std::set<std::string> competingKeywords;

        // 1. Coleta todas as keywords de categorias concorrentes
        for (const auto& pair : _categories) {
            const WeaponCategory& otherCategory = pair.second;

            // Uma categoria é "concorrente" se NÃO for a atual, mas tiver o MESMO tipo de arma base
            if (otherCategory.name != currentCategory->name &&
                otherCategory.equippedTypeValue == currentCategory->equippedTypeValue) {
                // Escolhe qual lista de keywords usar (direita ou esquerda) com base no contexto
                const auto& keywordsToExclude = isLeftHand ? otherCategory.leftHandKeywords : otherCategory.keywords;

                if (!keywordsToExclude.empty()) {
                    // Insere as keywords no set. Duplicatas serão ignoradas automaticamente.
                    competingKeywords.insert(keywordsToExclude.begin(), keywordsToExclude.end());
                }
            }
        }
        const auto& currentKeywords = isLeftHand ? currentCategory->leftHandKeywords : currentCategory->keywords;
        for (const auto& kw : currentKeywords) {
            competingKeywords.erase(kw);
        }
        // Se não houver keywords concorrentes, não há nada a fazer.
        if (competingKeywords.empty()) {
            return;
        }

        // 2. Cria um único bloco AND para conter todas as exclusões (NOT keyword1 AND NOT keyword2 ...)
        rapidjson::Value exclusionAndBlock(rapidjson::kObjectType);
        exclusionAndBlock.AddMember("condition", "AND", allocator);
        exclusionAndBlock.AddMember("comment", "Exclude competing weapon keywords", allocator);
        rapidjson::Value innerExclusionConditions(rapidjson::kArrayType);

        // 3. Itera sobre o SET de keywords únicas e adiciona a condição negada para cada uma
        for (const auto& keyword : competingKeywords) {
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
        // Adicione outros tipos se necessário
        return RuleType::GeneralNPC;  // Um padrão seguro
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

void CreatePerkListJson(rapidjson::Document& doc, rapidjson::Value& targetObject, const std::string& keyName,
                            const std::vector<PerkDef>& perkList) {
        if (perkList.empty()) return;

        rapidjson::Value perkArray(rapidjson::kArrayType);
        for (const auto& perk : perkList) {
            rapidjson::Value perkData(rapidjson::kArrayType);
            perkData.PushBack(rapidjson::Value(perk.pluginName.c_str(), doc.GetAllocator()), doc.GetAllocator());
            perkData.PushBack(perk.formID, doc.GetAllocator());
            perkData.PushBack(rapidjson::Value(perk.origin.c_str(), doc.GetAllocator()), doc.GetAllocator());
            perkArray.PushBack(perkData, doc.GetAllocator());
        }
        targetObject.AddMember(rapidjson::Value(keyName.c_str(), doc.GetAllocator()), perkArray, doc.GetAllocator());
    }

void ParsePerkListJson(const rapidjson::Value& sourceObject, const std::string& keyName, CategoryInstance* stance,
                           ModInstance* moveset, SubAnimationInstance* subMoveset) {
        if (!sourceObject.HasMember(keyName.c_str()) || !sourceObject[keyName.c_str()].IsArray()) {
            return;
        }

        const auto& perkArray = sourceObject[keyName.c_str()].GetArray();
        for (const auto& perkData : perkArray) {
            if (perkData.IsArray() && perkData.Size() == 3 && perkData[0].IsString() && perkData[1].IsUint() &&
                perkData[2].IsString()) {
                std::string plugin = perkData[0].GetString();
                RE::FormID formID = perkData[1].GetUint();
                std::string origin = perkData[2].GetString();

                PerkDef newPerk = {plugin, formID, origin};  // Cria a struct

                if (origin == "Stance" && stance)
                    stance->perkList.push_back(newPerk);
                else if (origin == "Moveset" && moveset)
                    moveset->perkList.push_back(newPerk);
                else if (origin == "SubMoveset" && subMoveset)
                    subMoveset->perkList.push_back(newPerk);
            }
        }
    }
void CreateEffectListJson(rapidjson::Document& doc, rapidjson::Value& targetObject, const std::string& keyName,
                          const std::vector<AppliedEffect>& effectList) {
    if (effectList.empty()) return;

    rapidjson::Value effectArray(rapidjson::kArrayType);
    auto& allocator = doc.GetAllocator();

    for (const auto& effect : effectList) {
        rapidjson::Value effectData(rapidjson::kArrayType);
        // Armazena o tipo como string para legibilidade
        const char* typeStr = "Unknown";
        switch (effect.type) {
            case AppliedEffect::EffectType::Perk:
                typeStr = "Perk";
                break;
            case AppliedEffect::EffectType::MagicEffect:
                typeStr = "MagicEffect";
                break;
            case AppliedEffect::EffectType::Spell:
                typeStr = "Spell";
                break;
        }
        effectData.PushBack(rapidjson::Value(typeStr, allocator), allocator);  // Índice 0: Tipo (string)
        effectData.PushBack(rapidjson::Value(effect.pluginName.c_str(), allocator), allocator);  // Índice 1: Plugin
        effectData.PushBack(effect.formID, allocator);                                           // Índice 2: FormID
        effectData.PushBack(rapidjson::Value(effect.origin.c_str(), allocator),
                            allocator);  // Índice 3: Origem (para UI)

        effectArray.PushBack(effectData, allocator);
    }
    targetObject.AddMember(rapidjson::Value(keyName.c_str(), allocator), effectArray, allocator);
}

// --- NOVO HELPER PARA CARREGAR ---
void ParseEffectListJson(const rapidjson::Value& sourceObject, const std::string& keyName,
                         std::vector<AppliedEffect>& targetList) {  // Modificado para receber a lista diretamente
    if (!sourceObject.HasMember(keyName.c_str()) || !sourceObject[keyName.c_str()].IsArray()) {
        return;
    }

    const auto& effectArray = sourceObject[keyName.c_str()].GetArray();
    for (const auto& effectData : effectArray) {
        // Verifica se tem 4 elementos e os tipos corretos
        if (effectData.IsArray() && effectData.Size() == 4 && effectData[0].IsString() && effectData[1].IsString() &&
            effectData[2].IsUint() && effectData[3].IsString()) {
            std::string typeStr = effectData[0].GetString();
            AppliedEffect::EffectType type = AppliedEffect::EffectType::Perk;  // Padrão
            if (typeStr == "MagicEffect")
                type = AppliedEffect::EffectType::MagicEffect;
            else if (typeStr == "Spell")
                type = AppliedEffect::EffectType::Spell;

            std::string plugin = effectData[1].GetString();
            RE::FormID formID = effectData[2].GetUint();
            std::string origin = effectData[3].GetString();

            targetList.push_back({type, plugin, formID, origin});  // Adiciona à lista fornecida
        }
    }
}

void CreateHitRulesJson(rapidjson::Document& doc, rapidjson::Value& targetObject, const std::string& keyName,
                        const std::vector<HitCountRule>& hitRulesList) {
    if (hitRulesList.empty()) return;

    rapidjson::Value rulesArray(rapidjson::kArrayType);
    auto& allocator = doc.GetAllocator();

    for (const auto& rule : hitRulesList) {
        rapidjson::Value ruleObj(rapidjson::kObjectType);
        ruleObj.AddMember("HitCount", rule.hitCount, allocator);

        // Reutiliza os helpers existentes para salvar as listas aninhadas
        CreatePerkListJsonFor2H(doc, ruleObj, "PerkList", rule.perks);
        CreateEffectListJson(doc, ruleObj, "AppliedEffects", rule.effects);

        rulesArray.PushBack(ruleObj, allocator);
    }
    targetObject.AddMember(rapidjson::Value(keyName.c_str(), allocator), rulesArray, allocator);
}

// --- ADICIONADO: Helper para CARREGAR HitRules ---
void ParseHitRulesJson(const rapidjson::Value& sourceObject, const std::string& keyName,
                       std::vector<HitCountRule>& targetList) {
    targetList.clear();  // Limpa a lista de destino
    if (!sourceObject.HasMember(keyName.c_str()) || !sourceObject[keyName.c_str()].IsArray()) {
        return;
    }

    const auto& rulesArray = sourceObject[keyName.c_str()].GetArray();
    for (const auto& ruleObj : rulesArray) {
        if (ruleObj.IsObject() && ruleObj.HasMember("HitCount") && ruleObj["HitCount"].IsInt()) {
            HitCountRule newRule;
            newRule.hitCount = ruleObj["HitCount"].GetInt();

            // Reutiliza os helpers existentes para carregar as listas aninhadas
            // Nota: Usamos ParsePerkListJsonFor2H (espera [plugin, formID])
            ParsePerkListJsonFor2H(ruleObj, "PerkList", newRule.perks);
            // Nota: Usamos ParseEffectListJson (espera [type, plugin, formID, origin])
            ParseEffectListJson(ruleObj, "AppliedEffects", newRule.effects);

            targetList.push_back(newRule);
        }
    }
}

    void AnimationManager::SaveCycleMovesets() {
        SKSE::log::info("Iniciando salvamento do estado da UI em arquivos User_CycleMoveset.json...");

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
                int stanceLimit = isPlayerRule ? category.instances.size() : 1;

                for (int i = 0; i < stanceLimit; ++i) {  // Stances
                    const CategoryInstance& instance = category.instances[i];
                    
                    for (size_t mod_idx = 0; mod_idx < instance.modInstances.size(); ++mod_idx) {
                        const auto& modInst = instance.modInstances[mod_idx];
                        if (!modInst.isSelected) {
                            continue;  // Pula movesets não selecionados ou vazios
                        }


                        const auto& sourceMod = _allMods[modInst.sourceModIndex];

                        int animationIndexCounter = 1;
                        for (const auto& subInst : modInst.subAnimationInstances) {
                            if (!subInst.isSelected) continue;

                            const auto& animOriginMod = _allMods[subInst.sourceModIndex];
                            const auto& animOriginSub = animOriginMod.subAnimations[subInst.sourceSubAnimIndex];
                            std::filesystem::path destJsonPath;
                            // Se a animação for do mod virtual DAR, o path é o próprio diretório
                            if (animOriginMod.name == "[DAR] Animations") {
                                destJsonPath = animOriginSub.path / "User_CycleMoveset.json";
                            } else {  // Senão, é o pai do config.json
                                destJsonPath = animOriginSub.path.parent_path() / "User_CycleMoveset.json";
                            }
                            requiredFiles.insert(destJsonPath);

                            if (documents.find(destJsonPath) == documents.end()) {
                                documents[destJsonPath] = std::make_unique<rapidjson::Document>();
                                documents[destJsonPath]->SetArray();
                            }
                            rapidjson::Document& doc = *documents[destJsonPath];
                            auto& allocator = doc.GetAllocator();

                            // 1. Encontra/Cria o Perfil do Ator
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

                            // 2. Encontra/Cria a Categoria
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
                           
                            // 3. Encontra/Cria a Stance (o moveset)
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

                                // <<< MUDANÇA PRINCIPAL: Usa o índice do loop (mod_idx) para definir a ordem
                                // Adicionamos +1 porque a ordem no JSON deve começar em 1, não em 0.
                                newStanceObj.AddMember("order", static_cast<int>(mod_idx + 1), allocator);
                                newStanceObj.AddMember("animations", rapidjson::kArrayType, allocator);
                                stancesArray.PushBack(newStanceObj, allocator);
                                stanceObj = &stancesArray.GetArray()[stancesArray.GetArray().Size() - 1];
                            }

                            // 4. Adiciona a Animação individual ao array "animations" da Stance
                            rapidjson::Value& animationsArray = (*stanceObj)["animations"];
                            rapidjson::Value animObj(rapidjson::kObjectType);
                            std::vector<PerkDef> allPerksForThisAnimation;
                            std::vector<AppliedEffect> allEffectsForThisAnimation;
                            for (const auto& eff : instance.appliedEffects) {
                                allEffectsForThisAnimation.push_back({eff.type, eff.pluginName, eff.formID, "Stance"});
                            }
                            // Coleta do Moveset
                            for (const auto& eff : modInst.appliedEffects) {
                                allEffectsForThisAnimation.push_back({eff.type, eff.pluginName, eff.formID, "Moveset"});
                            }
                            // Coleta do SubMoveset
                            for (const auto& eff : subInst.appliedEffects) {
                                allEffectsForThisAnimation.push_back(
                                    {eff.type, eff.pluginName, eff.formID, "SubMoveset"});
                            }

                            // Salva a lista consolidada (se não vazia)
                            if (!allEffectsForThisAnimation.empty()) {
                                CreateEffectListJson(doc, animObj, "AppliedEffects", allEffectsForThisAnimation);
                            }
                            for (const auto& p : instance.perkList) {
                                allPerksForThisAnimation.push_back({p.pluginName, p.formID, "Stance"});
                            }
                            for (const auto& p : modInst.perkList) {
                                allPerksForThisAnimation.push_back({p.pluginName, p.formID, "Moveset"});
                            }
                            for (const auto& p : subInst.perkList) {
                                allPerksForThisAnimation.push_back({p.pluginName, p.formID, "SubMoveset"});
                            }

                            if (!allPerksForThisAnimation.empty()) {
                                CreatePerkListJson(doc, animObj, "PerkList", allPerksForThisAnimation);
                            }

                            CreateHitRulesJson(doc, animObj, "StanceHitRules", instance.hitRules);
                            CreateHitRulesJson(doc, animObj, "MovesetHitRules", modInst.hitRules);
                            CreateHitRulesJson(doc, animObj, "SubMovesetHitRules", subInst.hitRules);

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

        // Processa o Player
        processActorCategories(_categories, nullptr);
        // Processa a Regra Geral
        processActorCategories(_generalNpcRule.categories, &_generalNpcRule);
        // Processa as Regras Específicas
        for (const auto& rule : _npcRules) {
            processActorCategories(rule.categories, &rule);
        }

        // Escreve os arquivos no disco
        SKSE::log::info("Escrevendo {} arquivos User_CycleMoveset.json...", documents.size());
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
                SKSE::log::error("Falha ao abrir para escrita: {}", path.string());
            }
        }

        // Limpa arquivos órfãos
        
        for (const auto& managedConfigPath : _managedFiles) {
            // Deriva o nome do arquivo de UI a partir do caminho do config.json gerenciado
            std::filesystem::path userCycleMovesetPath = managedConfigPath.parent_path() / "User_CycleMoveset.json";

            // Se o arquivo de UI não foi requerido nesta operação de salvamento, ele é um órfão.
            if (requiredFiles.find(userCycleMovesetPath) == requiredFiles.end()) {
                // Seja para limpar um arquivo existente ou criar um novo para sobrescrever um fallback,
                // a operação é a mesma: escrever "[]" no arquivo.
                SKSE::log::info("Limpando/Criando User_CycleMoveset.json órfão em: {}", userCycleMovesetPath.string());

                std::ofstream ofs(userCycleMovesetPath, std::ofstream::trunc);
                if (ofs) {
                    ofs << "[]";  // Escreve um array JSON vazio
                    ofs.close();
                } else {
                    SKSE::log::error("Falha ao abrir para limpar/criar o arquivo: {}", userCycleMovesetPath.string());
                }
            }
        }
        SKSE::log::info("Salvamento de {} arquivos User_CycleMoveset.json concluído.", documents.size());
    }


void AnimationManager::LoadCycleMovesets() {
    SKSE::log::info("Iniciando carregamento de regras dos arquivos (User_)CycleMoveset.json...");
    try {
        // Limpa o estado atual para garantir um carregamento limpo
        for (auto& pair : _categories) {
            for (auto& instance : pair.second.instances) {
                instance.modInstances.clear();
                instance.perkList.clear();  // Limpa perks do player para evitar contaminação inicial
            }
        }

        // Inicializa a regra geral de NPCs
        _generalNpcRule.categories = _categories;
        for (auto& pair : _generalNpcRule.categories) {
            for (auto& instance : pair.second.instances) {
                instance.modInstances.clear();
                instance.perkList.clear();  // <-- CORREÇÃO #1: Limpa os perks copiados
            }
        }
        _generalNpcRule.type = RuleType::GeneralNPC;
        _generalNpcRule.displayName = "NPCs (General)";
        _generalNpcRule.identifier = "GeneralNPC";
        _generalNpcRule.pluginName = "CMF Rule";
        _generalNpcRule.formID = 0xFFFFFFFF;
        _npcRules.clear();

        

        auto processJsonDocument = [&](rapidjson::Document& doc) {
            if (!doc.IsArray()) {
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
                    _generalNpcRule.formID = 0xFFFFFFFF;  // ID Sentinela
                } else {
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
                        newRule.categories = _categories;
                        for (auto& pair : newRule.categories) {
                            pair.second.ownerIsPlayer = false;
                            for (auto& instance : pair.second.instances) {
                                instance.modInstances.clear();
                                instance.perkList.clear();
                            }
                        }
                        _npcRules.push_back(newRule);
                        targetCategories = &_npcRules.back().categories;
                    } else {
                        targetCategories = &rule_it->categories;
                    }
                }

                if (!targetCategories) continue;

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
                        if (stanceIndex < 1) continue;  // Índices menores que 1 são sempre inválidos

                        // --- INÍCIO DA CORREÇÃO ---
                        // Verifica se o índice da stance está fora dos limites atuais do vetor
                        if (stanceIndex > categoryIt->second.instances.size()) {
                            // Se estiver, redimensiona os vetores para acomodar a nova stance (e as intermediárias)
                            size_t newSize = stanceIndex;
                            SKSE::log::info(
                                "Stance com índice {} encontrado para a categoria '{}'. Redimensionando de {} para {}.",
                                stanceIndex, categoryName, categoryIt->second.instances.size(), newSize);

                            categoryIt->second.instances.resize(newSize);
                            categoryIt->second.stanceNames.resize(newSize);
                            categoryIt->second.stanceNameBuffers.resize(newSize);

                            // Preenche os nomes e buffers das novas stances criadas com valores padrão
                            for (size_t i = 0; i < newSize; ++i) {
                                if (categoryIt->second.stanceNames[i].empty()) {  // Preenche apenas se estiver vazio
                                    std::string defaultName = std::format("Stance {}", i + 1);
                                    categoryIt->second.stanceNames[i] = defaultName;
                                    strcpy_s(categoryIt->second.stanceNameBuffers[i].data(),
                                             categoryIt->second.stanceNameBuffers[i].size(), defaultName.c_str());
                                }
                            }
                        }

                        CategoryInstance& targetInstance = categoryIt->second.instances[stanceIndex - 1];
                        std::string movesetName = stanceJson["name"].GetString();
                        auto modIdxOpt = FindModIndexByName(movesetName);
                        if (!modIdxOpt) {
                            SKSE::log::warn(
                                "Moveset com o nome '{}' não encontrado na biblioteca de animações. Verifique se o "
                                "nome no User_CycleMoveset.json corresponde ao nome no config.json do mod.",
                                movesetName);
                            continue;  // Pula este moveset
                        }

                        ModInstance* modInstancePtr = nullptr;
                        int hp = stanceJson.HasMember("hp") ? stanceJson["hp"].GetInt() : 100;
                        int st = stanceJson.HasMember("st") ? stanceJson["st"].GetInt() : 100;
                        int mn = stanceJson.HasMember("mn") ? stanceJson["mn"].GetInt() : 100;
                        int level = stanceJson.HasMember("level") ? stanceJson["level"].GetInt() : 0;
                        int order = stanceJson.HasMember("order") ? stanceJson["order"].GetInt() : 0;

                        for (auto& mi : targetInstance.modInstances) {
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
                            modInstancePtr->hp = hp;
                            modInstancePtr->st = st;
                            modInstancePtr->mn = mn;
                            modInstancePtr->level = level;
                            modInstancePtr->order = order;
                        }

                        for (const auto& animJson : stanceJson["animations"].GetArray()) {
                            if (!animJson.IsObject() || !animJson.HasMember("sourceConfigPath") ||
                                !animJson.HasMember("index"))
                                continue;

                            std::string configPathStr = animJson["sourceConfigPath"].GetString();
                            if (configPathStr.empty()) continue;

                            auto indicesOpt = FindSubAnimationByPath(std::filesystem::u8path(configPathStr));
                            if (!indicesOpt) {
                                SKSE::log::warn(
                                    "Não foi possível encontrar a animação para o config/path: {}. Pode ter sido "
                                    "removida. Pulando.",
                                    configPathStr);
                                continue;
                            }

                            SubAnimationInstance newSubInstance;
                            newSubInstance.sourceModIndex = indicesOpt->first;
                            newSubInstance.sourceSubAnimIndex = indicesOpt->second;
                            targetInstance.perkList.clear();
                            modInstancePtr->perkList.clear();
                            targetInstance.appliedEffects.clear();
                            modInstancePtr->appliedEffects.clear();
                            newSubInstance.appliedEffects.clear();
                            std::vector<AppliedEffect> loadedEffects;
                            ParseEffectListJson(animJson, "AppliedEffects", loadedEffects);

                            // Distribui os efeitos carregados para suas origens corretas
                            for (const auto& loadedEffect : loadedEffects) {
                                if (loadedEffect.origin == "Stance") {
                                    targetInstance.appliedEffects.push_back(loadedEffect);
                                } else if (loadedEffect.origin == "Moveset") {
                                    modInstancePtr->appliedEffects.push_back(loadedEffect);
                                } else if (loadedEffect.origin == "SubMoveset") {
                                    newSubInstance.appliedEffects.push_back(loadedEffect);
                                }
                            }
                            // Remove duplicatas em cada nível (opcional, mas bom)
                            std::sort(targetInstance.appliedEffects.begin(), targetInstance.appliedEffects.end());
                            targetInstance.appliedEffects.erase(
                                std::unique(targetInstance.appliedEffects.begin(), targetInstance.appliedEffects.end()),
                                targetInstance.appliedEffects.end());
                            // Faça o mesmo para modInstancePtr->appliedEffects e newSubInstance.appliedEffects
                            std::sort(modInstancePtr->appliedEffects.begin(), modInstancePtr->appliedEffects.end());
                            modInstancePtr->appliedEffects.erase(std::unique(modInstancePtr->appliedEffects.begin(),
                                                                             modInstancePtr->appliedEffects.end()),
                                                                 modInstancePtr->appliedEffects.end());
                            std::sort(newSubInstance.appliedEffects.begin(), newSubInstance.appliedEffects.end());
                            newSubInstance.appliedEffects.erase(
                                std::unique(newSubInstance.appliedEffects.begin(), newSubInstance.appliedEffects.end()),
                                newSubInstance.appliedEffects.end());
                            if (animJson.IsObject()) {
                                // Chama a nova função passando ponteiros para todos os níveis
                                ParsePerkListJson(animJson, "PerkList", &targetInstance, modInstancePtr,
                                                  &newSubInstance);
                            }
                            ParseHitRulesJson(animJson, "StanceHitRules", targetInstance.hitRules);
                            ParseHitRulesJson(animJson, "MovesetHitRules", modInstancePtr->hitRules);
                            ParseHitRulesJson(animJson, "SubMovesetHitRules", newSubInstance.hitRules);
                            if (animJson.HasMember("sourceSubName") && animJson["sourceSubName"].IsString()) {
                                const char* savedName = animJson["sourceSubName"].GetString();
                                const auto& originSubAnim = _allMods[newSubInstance.sourceModIndex]
                                                                .subAnimations[newSubInstance.sourceSubAnimIndex];
                                if (strcmp(savedName, originSubAnim.name.c_str()) != 0) {
                                    // Copia o nome salvo para o buffer da nova instância.
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

                            // --- FIM DA LÓGICA DE BUSCA MELHORADA ---

                            // (A lógica para preencher as flags pFront, pBack, etc. permanece a mesma)
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

                            newSubInstance.isSelected = true;  // Se está no arquivo, estava selecionada.

                            // (A lógica para inserir na posição correta via "index" permanece a mesma)
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

        const std::filesystem::path oarRootPath = "Data\\meshes\\actors\\character\\animations\\OpenAnimationReplacer";
        const std::filesystem::path darRootPath =
            "Data\\meshes\\actors\\character\\animations\\DynamicAnimationReplacer\\_CustomConditions";

        auto findAndProcessFiles = [&](const std::filesystem::path& root) {
            if (!std::filesystem::exists(root)) return;

            std::set<std::filesystem::path> processedFolders;

            for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
                if (!entry.is_directory()) continue;

                std::filesystem::path currentFolder = entry.path();
                if (processedFolders.count(currentFolder)) continue;

                if (std::filesystem::exists(currentFolder / "config.json") ||
                    std::filesystem::exists(currentFolder / "user.json")) {
                    processedFolders.insert(currentFolder);

                    // --- PASSO 1: FUSÃO CONDICIONAL ---
                    // Só executa se o cache for inválido. A única tarefa é criar/atualizar o User_CycleMoveset.json
                    if (_cacheWasInvalid) {
                        std::vector<std::filesystem::path> filesToMerge;
                        const std::string prefix = "User_CycleMoveset";
                        for (const auto& dirEntry : std::filesystem::directory_iterator(currentFolder)) {
                            if (dirEntry.is_regular_file() && dirEntry.path().filename().string().starts_with(prefix) &&
                                dirEntry.path().extension() == ".json") {
                                filesToMerge.push_back(dirEntry.path());
                            }
                        }

                        if (!filesToMerge.empty()) {
                            SKSE::log::info("Cache invalidado. Mesclando {} arquivos para {}", filesToMerge.size(),
                                            PathToUTF8(currentFolder / "User_CycleMoveset.json"));
                            rapidjson::Document masterDoc;
                            masterDoc.SetArray();
                            auto& allocator = masterDoc.GetAllocator();

                            for (const auto& filePath : filesToMerge) {
                                std::ifstream ifs(filePath);
                                if (!ifs.is_open()) continue;
                                std::string jsonContent((std::istreambuf_iterator<char>(ifs)), {});
                                ifs.close();

                                rapidjson::Document patchDoc;
                                patchDoc.Parse(jsonContent.c_str());

                                if (patchDoc.HasParseError() || !patchDoc.IsArray()) {
                                    SKSE::log::warn("Arquivo de patch mal formatado, pulando: {}", filePath.string());
                                    continue;
                                }

                                for (auto& newProfile : patchDoc.GetArray()) {
                                    if (!newProfile.IsObject() || !newProfile.HasMember("Identifier")) continue;
                                    const char* newProfileId = newProfile["Identifier"].GetString();

                                    rapidjson::Value* masterProfile = nullptr;
                                    for (auto& p : masterDoc.GetArray()) {
                                        if (p.IsObject() && p.HasMember("Identifier") &&
                                            strcmp(p["Identifier"].GetString(), newProfileId) == 0) {
                                            masterProfile = &p;
                                            break;
                                        }
                                    }

                                    if (!masterProfile) {
                                        rapidjson::Value newProfileCopy;
                                        newProfileCopy.CopyFrom(newProfile, allocator);
                                        masterDoc.PushBack(newProfileCopy, allocator);
                                    } else {
                                        if (newProfile.HasMember("Menu") && newProfile["Menu"].IsArray()) {
                                            for (auto& newCategory : newProfile["Menu"].GetArray()) {
                                                if (!newCategory.IsObject() || !newCategory.HasMember("Category"))
                                                    continue;
                                                const char* newCategoryName = newCategory["Category"].GetString();

                                                rapidjson::Value* masterCategory = nullptr;
                                                for (auto& c : (*masterProfile)["Menu"].GetArray()) {
                                                    if (c.IsObject() && c.HasMember("Category") &&
                                                        strcmp(c["Category"].GetString(), newCategoryName) == 0) {
                                                        masterCategory = &c;
                                                        break;
                                                    }
                                                }

                                                if (!masterCategory) {
                                                    rapidjson::Value newCategoryCopy;
                                                    newCategoryCopy.CopyFrom(newCategory, allocator);
                                                    (*masterProfile)["Menu"].PushBack(newCategoryCopy, allocator);
                                                } else {
                                                    if (newCategory.HasMember("stances") &&
                                                        newCategory["stances"].IsArray()) {
                                                        for (auto& newStance : newCategory["stances"].GetArray()) {
                                                            bool isDuplicate = false;
                                                            if (newStance.IsObject() && newStance.HasMember("name") &&
                                                                newStance.HasMember("index")) {
                                                                for (auto& masterStance :
                                                                     (*masterCategory)["stances"].GetArray()) {
                                                                    if (masterStance.IsObject() &&
                                                                        masterStance.HasMember("name") &&
                                                                        masterStance.HasMember("index") &&
                                                                        masterStance["index"].GetInt() ==
                                                                            newStance["index"].GetInt() &&
                                                                        strcmp(masterStance["name"].GetString(),
                                                                               newStance["name"].GetString()) == 0) {
                                                                        isDuplicate = true;
                                                                        break;
                                                                    }
                                                                }
                                                            }
                                                            if (!isDuplicate) {
                                                                rapidjson::Value newStanceCopy;
                                                                newStanceCopy.CopyFrom(newStance, allocator);
                                                                (*masterCategory)["stances"].PushBack(newStanceCopy,
                                                                                                      allocator);
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            if (masterDoc.IsArray() && !masterDoc.Empty()) {
                                std::filesystem::path canonicalPath = currentFolder / "User_CycleMoveset.json";
                                SKSE::log::info("Cache invalidado. Mesclando {} arquivos para {}", filesToMerge.size(),
                                                PathToUTF8(canonicalPath));
                                std::ofstream ofs(canonicalPath);
                                if (ofs) {
                                    rapidjson::StringBuffer buffer;
                                    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
                                    masterDoc.Accept(writer);
                                    ofs << buffer.GetString();
                                    ofs.close();
                                }
                                for (const auto& filePath : filesToMerge) {
                                    if (filePath != canonicalPath) {
                                        std::filesystem::remove(filePath);
                                    }
                                }
                            }
                        }
                    }

                    // --- PASSO 2: PROCESSAMENTO INCONDICIONAL ---
                    // Após a fusão (se necessária), esta parte carrega o arquivo de configuração final.
                    std::filesystem::path userFile = currentFolder / "User_CycleMoveset.json";
                    std::filesystem::path defaultFile = currentFolder / "CycleMoveset.json";
                    std::filesystem::path fileToProcess;

                    if (std::filesystem::exists(userFile)) {
                        fileToProcess = userFile;
                    } else if (std::filesystem::exists(defaultFile)) {
                        fileToProcess = defaultFile;
                    }

                    if (!fileToProcess.empty()) {
                        std::ifstream ifs(fileToProcess);
                        if (ifs.is_open()) {
                            std::string jsonContent((std::istreambuf_iterator<char>(ifs)), {});
                            rapidjson::Document doc;
                            doc.Parse(jsonContent.c_str());
                            if (!doc.HasParseError()) {
                                processJsonDocument(doc);
                            }
                        }
                    }
                }
            }
        };

        findAndProcessFiles(oarRootPath);
        findAndProcessFiles(darRootPath);

        // Ordena os movesets com base no campo "order" lido dos arquivos
        SKSE::log::info("Ordenando movesets com base na prioridade definida...");
        auto sortMovesets = [](std::map<std::string, WeaponCategory>& categories) {
            for (auto& categoryPair : categories) {
                for (auto& instance : categoryPair.second.instances) {
                    std::sort(instance.modInstances.begin(), instance.modInstances.end(),
                              [](const ModInstance& a, const ModInstance& b) { return a.order < b.order; });
                }
            }
        };
        sortMovesets(_categories);
        sortMovesets(_generalNpcRule.categories);
        for (auto& rule : _npcRules) {
            sortMovesets(rule.categories);
        }

        SKSE::log::info("Carregamento de regras concluído.");
        UpdateMaxMovesetCache();
    } catch (const std::filesystem::filesystem_error& e) {
        SKSE::log::critical("Erro de filesystem em LoadCycleMovesets: {}", e.what());
        RE::DebugNotification("CRITICAL ERROR loading moveset files! Check logs.");
    }
}



    void AnimationManager::SaveNPCSettings() {
        SKSE::log::info("Iniciando salvamento das configurações dos NPCs...");
        std::map<std::filesystem::path, std::vector<FileSaveConfig>> fileUpdates;

        for (auto& pair : _npcCategories) {
            WeaponCategory& category = pair.second;
            // NPCs usam apenas a stance 0
            CategoryInstance& instance = category.instances[0];
            int playlistParentCounter = 1;

            for (auto& modInstance : instance.modInstances) {
                if (!modInstance.isSelected) continue;

                for (auto& subInstance : modInstance.subAnimationInstances) {
                    if (!subInstance.isSelected) continue;

                    const auto& sourceMod = _allMods[subInstance.sourceModIndex];
                    const auto& sourceSubAnim = sourceMod.subAnimations[subInstance.sourceSubAnimIndex];

                    FileSaveConfig config;
                    config.isNPC = true;        // Define a flag para NPC
                    config.instance_index = 0;  // Stance 0 para NPCs
                    config.category = &category;
                    config.isParent = true;  // NPCs não têm direcionais, então tudo é "Pai"
                    config.order_in_playlist = playlistParentCounter++;

                    fileUpdates[sourceSubAnim.path].push_back(config);
                }
            }
        }

        SKSE::log::info("{} arquivos de configuração de NPC serão modificados.", fileUpdates.size());
        for (const auto& updateEntry : fileUpdates) {
            // A mesma função UpdateOrCreateJson é chamada!
            UpdateOrCreateJson(updateEntry.first, updateEntry.second);
        }
        RE::DebugNotification("Configurações dos NPCs salvas!");
    }

    void AnimationManager::AddKeywordCondition(rapidjson::Value& parentArray, const std::string& editorID, bool isLeftHand,
                                               bool negated, rapidjson::Document::AllocatorType& allocator) {
        if (editorID.empty()) return;  // Não faz nada se a keyword for vazia

        rapidjson::Value condition(rapidjson::kObjectType);
        condition.AddMember("condition", "IsEquippedHasKeyword", allocator);
        condition.AddMember("requiredVersion", "1.0.0.0", allocator);

        // Adiciona a flag "negated" se necessário
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
        // Se não há keywords para checar, não faz nada.
        if (keywords.empty()) {
            return;
        }

        // Se houver apenas UMA keyword, não precisamos de um bloco OR. Adicionamos a condição diretamente.
        if (keywords.size() == 1) {
            AddKeywordCondition(parentArray, keywords[0], isLeftHand, false, allocator);
            return;
        }

        // Se houver mais de uma keyword, criamos o bloco OR
        rapidjson::Value orBlock(rapidjson::kObjectType);
        orBlock.AddMember("condition", "OR", allocator);
        orBlock.AddMember("comment", "Matches any of the required keywords", allocator);

        rapidjson::Value innerOrConditions(rapidjson::kArrayType);

        // Adiciona cada keyword como uma condição dentro do bloco OR
        for (const auto& keyword : keywords) {
            AddKeywordCondition(innerOrConditions, keyword, isLeftHand, false, allocator);
        }

        orBlock.AddMember("Conditions", innerOrConditions, allocator);
        parentArray.PushBack(orBlock, allocator);
    }


    // Função para buscar o nome da stance
    std::string AnimationManager::GetStanceName(const std::string& categoryName, int stanceIndex) {
        auto it = _categories.find(categoryName);
        if (it != _categories.end()) {
            const auto& category = it->second;
            // --- CORREÇÃO AQUI ---
            // ANTES: if (stanceIndex < 0 || stanceIndex >= 4)
            // DEPOIS: Verifica contra o tamanho real do vetor de nomes
            if (stanceIndex < 0 || stanceIndex >= category.stanceNames.size()) {
                return "Stance Inválida";
            }
            return category.stanceNames[stanceIndex];
        }
        return std::to_string(stanceIndex + 1);  // Fallback
    }


    // NOVA FUNÇÃO: Busca as tags DPA e CPA para o moveset ativo (baseada em GetCurrentMovesetName)
    MovesetTags AnimationManager::GetCurrentMovesetTags(const std::string& categoryName,
                                                                          int stanceIndex, int movesetIndex) {
        auto animManager = AnimationManager::GetSingleton();
        if (movesetIndex <= 0) {
            return {false, false};  // Retorna padrão se não houver moveset ativo
        }

        auto cat_it = animManager->GetCategories().find(categoryName);
        if (cat_it == animManager->GetCategories().end()) {
            return {false, false};
        }

        const WeaponCategory& category = cat_it->second;
        if (stanceIndex < 0 || stanceIndex >= category.instances.size()) {
            return {false, false};
        }

        const CategoryInstance& instance = category.instances[stanceIndex];
        const SubAnimationInstance* targetMoveset = nullptr;
        int parentCounter = 0;

        // A lógica para encontrar o moveset ativo é a mesma da função GetCurrentMovesetName
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
                        goto found_target;  // Encontramos o moveset pai, podemos parar de procurar
                    }
                }
            }
        }

    found_target:
        if (targetMoveset) {
            // Retorna as tags do moveset encontrado
            return {targetMoveset->dpaTags, targetMoveset->hasCPA};
        }

        // Se não encontrou (índice inválido), retorna o padrão
        return {{}, false};
    }

    // Função para buscar o nome do moveset
    std::string AnimationManager::GetCurrentMovesetName(RE::Actor* actor, const std::string& categoryName,
                                                        int stanceIndex, int movesetIndex, int directionalState) {
        //SKSE::log::info("==========================================================");
        //SKSE::log::info("[GetCurrentMovesetName] Invocado com: Categoria='{}', Stance={}, MovesetIndex={}, Direcao={}",categoryName, stanceIndex, movesetIndex, directionalState);
        if (movesetIndex <= 0) {
            return "Nenhum";
        }

        auto cat_it = _categories.find(categoryName);
        if (cat_it == _categories.end()) {
            return "Categoria não encontrada";
        }

        WeaponCategory& category = cat_it->second;
        if (stanceIndex < 0 || stanceIndex >= category.instances.size()) {
            return "Stance inválida";
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
                        // Se não for necessária uma direção, já encontramos o que queríamos.
                        if (directionalState == 0) {
                            goto found_target;
                        }
                        // Caso contrário, continuamos escaneando em busca de filhos.
                    } else if (targetParent != nullptr) {
                        // Se já passamos pelo nosso "pai" alvo e encontramos o PRÓXIMO "pai",
                        // significa que não havia um filho direcional válido no meio. Paramos a busca.
                        goto found_target;
                    }
                } else if (targetParent != nullptr && directionalState != 0) {
                    // Se estamos nesta parte, significa que encontramos uma animação "filha" E
                    // já passamos pelo "pai" que estávamos procurando.
                    bool isDirectionalMatch =
                        (directionalState == 1 && subInst.pFront) || (directionalState == 2 && subInst.pFrontRight) ||
                        (directionalState == 3 && subInst.pRight) || (directionalState == 4 && subInst.pBackRight) ||
                        (directionalState == 5 && subInst.pBack) || (directionalState == 6 && subInst.pBackLeft) ||
                        (directionalState == 7 && subInst.pLeft) || (directionalState == 8 && subInst.pFrontLeft);

                    if (isDirectionalMatch) {
                        if (CheckActorHasPerks(actor, subInst.perkList)) {
                            // Se o jogador tem o perk, retorna o nome do filho
                            const auto& sourceSubAnimChild =
                                _allMods[subInst.sourceModIndex].subAnimations[subInst.sourceSubAnimIndex];
                            return (subInst.editedName[0] != '\0') ? subInst.editedName.data()
                                                                   : sourceSubAnimChild.name;
                        }
                        // Encontramos o filho direcional que corresponde ao nosso pai! Este é o resultado final.
                        const auto& sourceSubAnimChild =
                            _allMods[subInst.sourceModIndex].subAnimations[subInst.sourceSubAnimIndex];
                        return (subInst.editedName[0] != '\0') ? subInst.editedName.data() : sourceSubAnimChild.name;
                    }
                }
            }
        }

    found_target:
        // Se chegamos aqui, ou encontramos o pai e não precisávamos de direção, ou não encontramos um filho válido.
        // Em ambos os casos, retornamos o nome do pai.
        if (targetParent) {
            const auto& sourceSubAnimParent =
                _allMods[targetParent->sourceModIndex].subAnimations[targetParent->sourceSubAnimIndex];
            return (targetParent->editedName[0] != '\0') ? targetParent->editedName.data() : sourceSubAnimParent.name;
        }

        // Se a função terminar aqui, o movesetIndex era inválido (ex: pediu o 5º pai, mas só existem 4).
        //SKSE::log::warn("[GetCurrentMovesetName] Nenhum moveset 'pai' encontrado para o índice {}", movesetIndex);
        return "Não encontrado";
    }


    void AnimationManager::SaveStances() {
        SKSE::log::info("Salvando stances em arquivos JSON individuais por categoria...");
        const std::filesystem::path stancesFolderPath = "Data/SKSE/Plugins/CycleMovesets/Stances";

        try {
            if (!std::filesystem::exists(stancesFolderPath)) {
                std::filesystem::create_directories(stancesFolderPath);
            }
        } catch (const std::filesystem::filesystem_error& e) {
            SKSE::log::error("Falha ao criar o diretório raiz de stances: {}. Erro: {}", stancesFolderPath.string(),
                             e.what());
            return;
        }

        std::set<std::filesystem::path> validFiles;

        for (const auto& pair : _categories) {
            const WeaponCategory& category = pair.second;
            std::filesystem::path categoryStancePath = stancesFolderPath / category.name;

            try {
                if (!std::filesystem::exists(categoryStancePath)) {
                    std::filesystem::create_directories(categoryStancePath);
                }
            } catch (const std::filesystem::filesystem_error& e) {
                SKSE::log::error("Falha ao criar o diretório de stance para a categoria {}: {}", category.name,
                                 e.what());
                continue;
            }

            for (int i = 0; i < category.instances.size(); ++i) {
                const auto& instance = category.instances[i];
                const std::string& stanceName = category.stanceNames[i];
                std::filesystem::path stanceFilePath = categoryStancePath / (stanceName + ".json");

                // Adiciona ao set de arquivos válidos que devem existir
                validFiles.insert(stanceFilePath);

                rapidjson::Document doc;
                doc.SetObject();
                auto& allocator = doc.GetAllocator();

                doc.AddMember("name", rapidjson::Value(stanceName.c_str(), allocator), allocator);
                doc.AddMember("index", i, allocator);

                // Salva os perks de requisito (usando a função 2HHandle que salva [plugin, formID])
                CreatePerkListJsonFor2H(doc, doc, "perksToUse", instance.perkList);

                // Salva os efeitos aplicados (usando a função que salva [type, plugin, formID, origin])
                // O "origin" será salvo como "" (vazio), o que é bom.
                CreateEffectListJson(doc, doc, "effectsToApply", instance.appliedEffects);

                std::ofstream ofs(stanceFilePath);
                if (!ofs) {
                    SKSE::log::error("Falha ao abrir {} para escrita!", stanceFilePath.string());
                    continue;
                }

                rapidjson::StringBuffer buffer;
                rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
                doc.Accept(writer);
                ofs << buffer.GetString();
                ofs.close();
            }

            // Limpa arquivos órfãos (stances deletadas ou renomeadas)
            try {
                if (std::filesystem::exists(categoryStancePath)) {
                    for (const auto& entry : std::filesystem::directory_iterator(categoryStancePath)) {
                        if (entry.is_regular_file() && entry.path().extension() == ".json") {
                            if (validFiles.find(entry.path()) == validFiles.end()) {
                                SKSE::log::info("Removendo arquivo de stance órfão: {}", entry.path().string());
                                std::filesystem::remove(entry.path());
                            }
                        }
                    }
                }
            } catch (const std::filesystem::filesystem_error& e) {
                SKSE::log::error("Erro ao limpar arquivos de stance órfãos em {}: {}", categoryStancePath.string(),
                                 e.what());
            }
        }

        SKSE::log::info("Salvamento das stances individuais concluído.");
    }

    void AnimationManager::LoadStances() {
        SKSE::log::info("Carregando nomes/dados das stances...");
        const std::filesystem::path rootStancesPath = "Data/SKSE/Plugins/CycleMovesets/Stances";

        if (!std::filesystem::exists(rootStancesPath)) {
            SKSE::log::info("Diretório de nomes de stance não encontrado. Usando padrões.");
            return;
        }

        for (auto& pair : _categories) {
            WeaponCategory& category = pair.second;
            std::filesystem::path oldFilePath = rootStancesPath / (category.name + ".json");
            std::filesystem::path newFolderPath = rootStancesPath / category.name;

            // --- 1. LÓGICA DE MIGRAÇÃO ---
            if (std::filesystem::exists(oldFilePath) && std::filesystem::is_regular_file(oldFilePath)) {
                SKSE::log::warn("Arquivo de stance no formato antigo encontrado para {}. Iniciando migração...",
                                category.name);
                std::ifstream ifs(oldFilePath);
                if (!ifs) {
                    SKSE::log::error("Falha ao abrir arquivo antigo {} para migração.", oldFilePath.string());
                    continue;
                }
                std::string jsonContent((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                ifs.close();

                rapidjson::Document doc;
                doc.Parse(jsonContent.c_str());

                if (!doc.HasParseError() && doc.IsArray()) {
                    try {
                        // Cria o novo diretório
                        if (!std::filesystem::exists(newFolderPath)) {
                            std::filesystem::create_directories(newFolderPath);
                        }

                        const auto& stanceNamesArray = doc.GetArray();
                        size_t numStances = stanceNamesArray.Size();
                        if (numStances == 0) numStances = 4;

                        // Redimensiona os vetores da categoria
                        category.instances.resize(numStances);
                        category.stanceNames.resize(numStances);
                        category.stanceNameBuffers.resize(numStances);

                        // Cria os novos arquivos JSON
                        for (rapidjson::SizeType i = 0; i < numStances; ++i) {
                            std::string stanceName;
                            if (i < stanceNamesArray.Size() && stanceNamesArray[i].IsString()) {
                                stanceName = stanceNamesArray[i].GetString();
                            } else {
                                stanceName = std::format("Stance {}", i + 1);
                            }

                            category.stanceNames[i] = stanceName;
                            strcpy_s(category.stanceNameBuffers[i].data(), category.stanceNameBuffers[i].size(),
                                     stanceName.c_str());

                            // Limpa dados antigos (migração não preserva perks/efeitos, pois eles não estavam aqui)
                            category.instances[i].perkList.clear();
                            category.instances[i].appliedEffects.clear();

                            // Cria o novo arquivo JSON da stance
                            std::filesystem::path newStancePath = newFolderPath / (stanceName + ".json");
                            rapidjson::Document newDoc;
                            newDoc.SetObject();
                            auto& allocator = newDoc.GetAllocator();
                            newDoc.AddMember("name", rapidjson::Value(stanceName.c_str(), allocator), allocator);
                            newDoc.AddMember("index", static_cast<int>(i), allocator);
                            newDoc.AddMember("perksToUse", rapidjson::kArrayType, allocator);
                            newDoc.AddMember("effectsToApply", rapidjson::kArrayType, allocator);

                            std::ofstream ofs(newStancePath);
                            if (ofs) {
                                rapidjson::StringBuffer buffer;
                                rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
                                newDoc.Accept(writer);
                                ofs << buffer.GetString();
                                ofs.close();
                            }
                        }

                        // Deleta o arquivo antigo
                        std::filesystem::remove(oldFilePath);
                        SKSE::log::info("Migração da categoria {} concluída com sucesso.", category.name);

                    } catch (const std::filesystem::filesystem_error& e) {
                        SKSE::log::error("Falha de filesystem durante a migração de {}: {}", category.name, e.what());
                    }
                }
            }
            // --- 2. LÓGICA DE CARREGAMENTO NORMAL (NOVO FORMATO) ---
            else if (std::filesystem::exists(newFolderPath) && std::filesystem::is_directory(newFolderPath)) {
                // Usamos um tuple para carregar e depois ordenar pelo índice
                std::vector<std::tuple<int, std::string, std::vector<PerkDef>, std::vector<AppliedEffect>>>
                    loadedStances;

                try {
                    for (const auto& entry : std::filesystem::directory_iterator(newFolderPath)) {
                        if (entry.is_regular_file() && entry.path().extension() == ".json") {
                            std::ifstream ifs(entry.path());
                            if (!ifs) {
                                SKSE::log::error("Falha ao abrir arquivo de stance: {}", entry.path().string());
                                continue;
                            }
                            std::string jsonContent((std::istreambuf_iterator<char>(ifs)),
                                                    std::istreambuf_iterator<char>());
                            ifs.close();

                            rapidjson::Document doc;
                            doc.Parse(jsonContent.c_str());

                            if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("index") ||
                                !doc.HasMember("name")) {
                                SKSE::log::error("Erro no parse do JSON ou arquivo de stance malformado: {}",
                                                 entry.path().string());
                                continue;
                            }

                            int index = doc["index"].GetInt();
                            std::string name = doc["name"].GetString();
                            std::vector<PerkDef> perks;
                            std::vector<AppliedEffect> effects;

                            // Carrega os perks (usando o helper simples)
                            ParsePerkListJsonFor2H(doc, "perksToUse", perks);
                            // Carrega os efeitos (o helper existente funciona)
                            ParseEffectListJson(doc, "effectsToApply", effects);

                            loadedStances.emplace_back(index, name, perks, effects);
                        }
                    }
                } catch (const std::filesystem::filesystem_error& e) {
                    SKSE::log::error("Erro de filesystem ao ler stances de {}: {}", newFolderPath.string(), e.what());
                }

                if (loadedStances.empty()) {
                    // Se a pasta existir mas estiver vazia, usa os padrões (4 stances)
                    category.instances.resize(1);
                    category.stanceNames.resize(1);
                    category.stanceNameBuffers.resize(1);
                    for (int i = 0; i < 1; ++i) {
                        category.stanceNames[i] = std::format("Stance {}", i + 1);
                        strcpy_s(category.stanceNameBuffers[i].data(), category.stanceNameBuffers[i].size(),
                                 category.stanceNames[i].c_str());
                    }
                    continue;  // Pula para a próxima categoria
                }

                // Ordena as stances pelo índice salvo
                std::sort(loadedStances.begin(), loadedStances.end(),
                          [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });

                size_t numStances = loadedStances.size();
                category.instances.resize(numStances);
                category.stanceNames.resize(numStances);
                category.stanceNameBuffers.resize(numStances);

                // Popula os dados da categoria na ordem correta
                for (size_t i = 0; i < numStances; ++i) {
                    category.stanceNames[i] = std::get<1>(loadedStances[i]);
                    strcpy_s(category.stanceNameBuffers[i].data(), category.stanceNameBuffers[i].size(),
                             category.stanceNames[i].c_str());
                    category.instances[i].perkList = std::get<2>(loadedStances[i]);
                    category.instances[i].appliedEffects = std::get<3>(loadedStances[i]);
                }
            }
            // Se nem o arquivo antigo nem a pasta nova existirem, a categoria usará os 4 valores padrão
            // que foram inicializados em ScanAnimationMods.
        }

        SKSE::log::info("Carregamento de dados das stances concluído.");
    }

    void AnimationManager::DrawStanceManagementPopup() {
        if (_isStanceManagementPopupOpen) {
            ImGui::OpenPopup("Edit Stance##ManagementPopup");
        }

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 center = ImVec2(viewport->Pos.x + viewport->Size.x * 0.6f, viewport->Pos.y + viewport->Size.y * 0.6f);
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        if (ImGui::BeginPopupModal("Edit Stance##ManagementPopup", &_isStanceManagementPopupOpen,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            // Inicializa o buffer de input do índice na primeira vez que o popup aparece
            if (ImGui::IsWindowAppearing()) {
                if (_categoryToEdit) {                          // Garante que o ponteiro não é nulo
                    _stanceNewIndexInput = _stanceIndexToEdit;  // Usa 0-based internamente
                }
            }

            if (!_categoryToEdit || _stanceIndexToEdit == -1 ||
                _stanceIndexToEdit >= _categoryToEdit->instances.size()) {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                return;
            }

            CategoryInstance& instance = _categoryToEdit->instances[_stanceIndexToEdit];
            std::string stanceName = _categoryToEdit->stanceNames[_stanceIndexToEdit];
            size_t totalStances = _categoryToEdit->instances.size();

            ImGui::Text("Managing Stance: %s", stanceName.c_str());
            ImGui::Separator();
            ImGui::Spacing();

            // 1. Botão de Editar Nome
            if (ImGui::Button("Edit Name", ImVec2(250, 0))) {
                _isStanceManagementPopupOpen = false;
                strcpy_s(_editStanceNameBuffer, sizeof(_editStanceNameBuffer), stanceName.c_str());
                _isEditStanceModalOpen = true;
                ImGui::CloseCurrentPopup();  // Fecha este popup para abrir o outro
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Change the display name of this stance.");

            // 2. Botão de Condições & Efeitos
            std::string condEffLabel = std::format("(C:{} / E:{})", instance.perkList.size(),
                                                   instance.appliedEffects.size());
            if (ImGui::Button(condEffLabel.c_str(), ImVec2(200, 0))) {
                _isStanceManagementPopupOpen = false;
                _perksToDisplayInPopup.clear();
                _inheritedPerkFormIDs.clear();
                _effectsToDisplayInPopup.clear();
                _inheritedEffectFormIDs.clear();
                _inheritedHitRules.clear();
                _perksToDisplayInPopup = instance.perkList;
                _effectsToDisplayInPopup = instance.appliedEffects;
                _stanceToEditPerk = &instance;
                _movesetToEditPerk = nullptr;
                _subMovesetToEditPerk = nullptr;
                _stanceToEditEffect = &instance;
                _movesetToEditEffect = nullptr;
                _subMovesetToEditEffect = nullptr;
                _editingPlayer2HPerks = false;
                _editingNPC2HPerks = false;
                _editingNPCDual2HPerks = false;

                _isConditionsEffectsPopupOpen = true;
                ImGui::CloseCurrentPopup();  // Fecha este popup para abrir o outro
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Perks requeriment to unlock the stance and effects");
            ImGui::Separator();
            ImGui::Spacing();

            // 3. NOVO: Lógica de Input Direto de Índice
            ImGui::BeginGroup();
            // Mostra o índice 1-based para o usuário
            ImGui::Text("Current Index: %d", _stanceIndexToEdit + 1);

            ImGui::PushItemWidth(150);
            // O input também é 1-based
            int displayIndex = _stanceNewIndexInput + 1;
            if (ImGui::InputInt("Set Index", &displayIndex, 1, 0)) {
                _stanceNewIndexInput = displayIndex - 1;  // Converte de volta para 0-based
            }
            ImGui::PopItemWidth();
            ImGui::SameLine();
            if (ImGui::Button("Set", ImVec2(60, 0))) {
                int currentIndex = _stanceIndexToEdit;
                int newIndex = _stanceNewIndexInput;

                // Validação (0-based)
                if (newIndex < 0) newIndex = 0;
                if (newIndex >= totalStances) newIndex = totalStances - 1;

                if (newIndex != currentIndex) {
                    // Salva os dados da stance que vamos mover
                    auto instanceToMove = std::move(_categoryToEdit->instances[currentIndex]);
                    auto nameToMove = std::move(_categoryToEdit->stanceNames[currentIndex]);
                    auto bufferToMove = std::move(_categoryToEdit->stanceNameBuffers[currentIndex]);

                    // Remove da posição antiga
                    _categoryToEdit->instances.erase(_categoryToEdit->instances.begin() + currentIndex);
                    _categoryToEdit->stanceNames.erase(_categoryToEdit->stanceNames.begin() + currentIndex);
                    _categoryToEdit->stanceNameBuffers.erase(_categoryToEdit->stanceNameBuffers.begin() + currentIndex);

                    // Insere na nova posição
                    _categoryToEdit->instances.insert(_categoryToEdit->instances.begin() + newIndex,
                                                      std::move(instanceToMove));
                    _categoryToEdit->stanceNames.insert(_categoryToEdit->stanceNames.begin() + newIndex,
                                                        std::move(nameToMove));
                    _categoryToEdit->stanceNameBuffers.insert(_categoryToEdit->stanceNameBuffers.begin() + newIndex,
                                                              std::move(bufferToMove));
                    _categoryToEdit->activeInstanceIndex = newIndex;
                    _categoryToEdit->uiTabVersion++;
                }
                _isStanceManagementPopupOpen = false;
                ImGui::CloseCurrentPopup();  // Fecha para forçar o redraw

            }
            ImGui::EndGroup();

            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Set the order index (1-based).\nMin: 1, Max: %zu", totalStances);

            ImGui::Separator();
            ImGui::Spacing();

            // 4. Botão de Deletar
            if (_categoryToEdit->instances.size() <= 1) ImGui::BeginDisabled();
            if (ImGui::Button("Delete Stance", ImVec2(250, 0))) {
                ImGui::OpenPopup("Confirm Stance Deletion");  // Abre o popup de confirmação
            }
            if (_categoryToEdit->instances.size() <= 1) ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Removes this stance permanently. A category must have at least one stance.");

            ImGui::Spacing();
            ImGui::Separator();

            // Botão de Fechar o popup de gerenciamento
            /*if (ImGui::Button("Close", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }*/

            // --- Pop-up de Confirmação de Deleção (aninhado) ---
            if (ImGui::BeginPopupModal("Confirm Stance Deletion", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("Are you sure you want to delete this stance?\nAll movesets inside it will be lost.");
                if (ImGui::Button("Yes, Delete It", ImVec2(200, 0))) {
                    if (_categoryToEdit && _stanceIndexToEdit != -1 && _categoryToEdit->instances.size() > 1) {
                        _categoryToApplyDeletion = _categoryToEdit;
                        _stanceIndexToDelete = _stanceIndexToEdit;
                    }
                    ImGui::CloseCurrentPopup();  // Fecha o "Confirm"
                    ImGui::CloseCurrentPopup();  // Fecha o "Management"
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            ImGui::EndPopup();
        }
    }

    void AnimationManager::DrawStanceEditorPopup() {
        if (_isEditStanceModalOpen) {
            ImGui::OpenPopup(LOC("edit_stance_name_popup"));
            _isEditStanceModalOpen = false;  // Reseta o gatilho
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
                    // Salva o nome do buffer temporário para a estrutura de dados real
                    _categoryToEdit->stanceNames[_stanceIndexToEdit] = _editStanceNameBuffer;
                    // E também para o buffer que a Tab usa, para atualização visual instantânea
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
        // Se a flag for verdadeira, nós dizemos ao ImGui para abrir o popup na próxima frame
        if (_showRestartPopup) {
            ImGui::OpenPopup("Restart Required");
            _showRestartPopup = false;  // Reseta a flag para não abrir toda hora
        }

        // Configura a posição central do popup
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 center = ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f);
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        // Define o conteúdo do popup
        if (ImGui::BeginPopupModal("Restart Required", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Configs saved, reload the game to take effect.");
            ImGui::Separator();

            // Centraliza o botão OK
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

        SKSE::log::info("Iniciando salvamento do moveset do usuário: {}", movesetName);
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
            SKSE::log::error("Falha ao criar a pasta do moveset: {}. Erro: {}", newMovesetPath.string(), e.what());
            RE::DebugNotification("ERROR: Failed to create moveset folder!");
            return;
        }

        // NOVA ESTRUTURA PARA AGRUPAR DADOS DE SALVAMENTO
        struct SubmovesetSaveData {
            // Armazena todas as instâncias que compartilham o mesmo nome editado
            std::vector<const CreatorSubAnimationInstance*> instances;
            std::vector<FileSaveConfig> configs;
        };
        std::map<std::string, SubmovesetSaveData> uniqueSubmovesets;

        for (WeaponCategory* cat : selectedCategories) {
            if (_movesetCreatorStances.find(cat->name) == _movesetCreatorStances.end()) continue;

            auto& stances = _movesetCreatorStances.at(cat->name);
            for (int i = 0; i < 4; ++i) {
                // A flag _newMovesetStanceEnabled foi removida pois não existia no código original,
                // a verificação será feita pela existência de submovesets na stance.
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

                    // ===== INÍCIO DA CORREÇÃO =====
                    // Adiciona as informações da regra do Player ao config.
                    // Movesets criados por esta ferramenta são sempre para o Player.
                    config.ruleType = RuleType::Player;
                    config.formID = 0x7;
                    config.pluginName = "Skyrim.esm";
                    config.ruleIdentifier = "Player";
                    // ===== FIM DA CORREÇÃO =====

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

            // LÓGICA ATUALIZADA PARA O CYCLEDAR.JSON
            {
                rapidjson::Document cycleDoc;
                cycleDoc.SetObject();
                auto& allocator = cycleDoc.GetAllocator();

                rapidjson::Value sourcesArray(rapidjson::kArrayType);
                bool anyBfco = false;

                // ===== INÍCIO DA CORREÇÃO =====
                // 1. Cria um set para armazenar os paths únicos que já foram adicionados a este JSON.
                std::set<std::string> uniquePaths;
                // ===== FIM DA CORREÇÃO =====

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

                    // ===== INÍCIO DA CORREÇÃO =====
                    // 2. Tenta inserir o path no set. O bloco if só será executado se o path for novo.
                    if (uniquePaths.insert(originalPathStr).second) {
                        // ===== FIM DA CORREÇÃO =====

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
                cycleMovesetDoc.SetArray();  // A raiz é um array
                auto& allocator = cycleMovesetDoc.GetAllocator();

                // 1. Cria o objeto de Perfil para o Player
                rapidjson::Value profileObj(rapidjson::kObjectType);
                profileObj.AddMember("Type", "Player", allocator);
                profileObj.AddMember("Name", "Player", allocator);
                profileObj.AddMember("FormID", "00000007", allocator);
                profileObj.AddMember("Plugin", "Skyrim.esm", allocator);
                profileObj.AddMember("Identifier", "Player", allocator);

                rapidjson::Value menuArray(rapidjson::kArrayType);

                // 2. Agrupa as configurações por Categoria
                std::map<std::string, std::vector<const FileSaveConfig*>> configsByCategory;
                for (const auto& config : data.configs) {
                    configsByCategory[config.category->name].push_back(&config);
                }

                // 3. Itera sobre cada Categoria
                for (const auto& catPair : configsByCategory) {
                    rapidjson::Value categoryObj(rapidjson::kObjectType);
                    categoryObj.AddMember("Category", rapidjson::Value(catPair.first.c_str(), allocator), allocator);
                    rapidjson::Value stancesArray(rapidjson::kArrayType);

                    // 4. Agrupa as configurações por Stance
                    std::map<int, std::vector<const FileSaveConfig*>> configsByStance;
                    for (const auto* configPtr : catPair.second) {
                        configsByStance[configPtr->instance_index].push_back(configPtr);
                    }

                    // 5. Itera sobre cada Stance
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

                        // 6. Adiciona cada animação (sub-moveset) à Stance
                        for (const auto* configPtr : stancePair.second) {
                            rapidjson::Value animObj(rapidjson::kObjectType);
                            animObj.AddMember("index", configPtr->order_in_playlist, allocator);
                            animObj.AddMember("sourceModName", rapidjson::Value(movesetName.c_str(), allocator),
                                              allocator);
                            animObj.AddMember("sourceSubName", rapidjson::Value(subName.c_str(), allocator), allocator);

                            // O caminho do config.json que a função Load irá procurar
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

                // 7. Salva o arquivo final
                std::ofstream outFile(subMovesetPath / "CycleMoveset.json");
                rapidjson::StringBuffer buffer;
                rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
                cycleMovesetDoc.Accept(writer);
                outFile << buffer.GetString();
            }
            
        }

        _newMovesetCategorySelection.clear();
        _movesetCreatorStances.clear();

        SKSE::log::info("Salvamento do moveset '{}' concluído.", movesetName);
        RE::DebugNotification(std::format("Moveset '{}' salvo com sucesso!", movesetName).c_str());
        _showRestartPopup = true;
    }


    void AnimationManager::ScanDarAnimations() {
        SKSE::log::info("[ScanDarAnimations] Iniciando a função de escaneamento DAR.");
        try {
            _darSubMovesets.clear();
            SKSE::log::info("[ScanDarAnimations] Vetor _darSubMovesets foi limpo.");

            const std::filesystem::path darRootPath =
                "Data\\meshes\\actors\\character\\animations\\DynamicAnimationReplacer\\_CustomConditions";

            // Convertendo para std::string para o log
            auto u8_darRootPath = darRootPath.u8string();
            SKSE::log::info("[ScanDarAnimations] Caminho a ser verificado: {}",
                            std::string(u8_darRootPath.begin(), u8_darRootPath.end()));

            if (!std::filesystem::exists(darRootPath) || !std::filesystem::is_directory(darRootPath)) {
                SKSE::log::warn("[ScanDarAnimations] A pasta raiz do DAR (_CustomConditions) não foi encontrada em '{}'.",
                                std::string(u8_darRootPath.begin(), u8_darRootPath.end()));
                RE::DebugNotification("Pasta do DAR (_CustomConditions) não encontrada.");
                return;
            }

            SKSE::log::info("[ScanDarAnimations] Pasta encontrada. Iniciando iteração pelas subpastas...");
            int folderCount = 0;
            for (const auto& entry : std::filesystem::directory_iterator(darRootPath)) {
                folderCount++;

                auto u8_entryPath = entry.path().u8string();
                SKSE::log::info("[ScanDarAnimations] [LOOP {}] Verificando a entrada: '{}'", folderCount,
                                std::string(u8_entryPath.begin(), u8_entryPath.end()));

                if (entry.is_directory()) {
                    SKSE::log::info("[ScanDarAnimations] [LOOP {}] É um diretório. Processando...", folderCount);

                    SubAnimationDef subAnimDef;

                    SKSE::log::info("[ScanDarAnimations] [LOOP {}] Extraindo nome da pasta...", folderCount);

                    // ===================================================================
                    // CORREÇÃO PRINCIPAL: Converter explicitamente std::u8string para std::string
                    // ===================================================================
                    auto u8_filename = entry.path().filename().u8string();
                    subAnimDef.name = std::string(u8_filename.begin(), u8_filename.end());
                    SKSE::log::info("[ScanDarAnimations] [LOOP {}] Nome extraído: '{}'", folderCount, subAnimDef.name);

                    subAnimDef.path = entry.path();
                    auto u8_subAnimPath = subAnimDef.path.u8string();
                    SKSE::log::info("[ScanDarAnimations] [LOOP {}] Path definido como: '{}'", folderCount,
                                    std::string(u8_subAnimPath.begin(), u8_subAnimPath.end()));

                    SKSE::log::info("[ScanDarAnimations] [LOOP {}] Chamando ScanSubAnimationFolderForTags para '{}'...",
                                    folderCount, subAnimDef.name);
                    ScanSubAnimationFolderForTags(entry.path(), subAnimDef);
                    SKSE::log::info(
                        "[ScanDarAnimations] [LOOP {}] Retornou de ScanSubAnimationFolderForTags. A pasta tem animações: "
                        "{}",
                        folderCount, subAnimDef.hasAnimations);

                    if (subAnimDef.hasAnimations) {
                        SKSE::log::info("[ScanDarAnimations] [LOOP {}] O submoveset tem animações. Adicionando ao vetor...",
                                        folderCount);
                        _darSubMovesets.push_back(subAnimDef);
                        SKSE::log::info("[ScanDarAnimations] [LOOP {}] Adicionado com sucesso: '{}'", folderCount,
                                        subAnimDef.name);
                    } else {
                        SKSE::log::info(
                            "[ScanDarAnimations] [LOOP {}] O submoveset '{}' não contém arquivos .hkx e será pulado.",
                            folderCount, subAnimDef.name);
                    }
                } else {
                    SKSE::log::info("[ScanDarAnimations] [LOOP {}] A entrada não é um diretório. Pulando.", folderCount);
                }
            }
        } catch (const std::filesystem::filesystem_error& e) {
            SKSE::log::critical("[ScanDarAnimations] CRASH! ERRO DE FILESYSTEM DURANTE O SCAN: {}", e.what());
            RE::DebugNotification("ERRO GRAVE ao ler pastas DAR! Verifique os logs.");
        } catch (const std::exception& e) {
            SKSE::log::critical("[ScanDarAnimations] CRASH! ERRO GERAL DURANTE O SCAN: {}", e.what());
            RE::DebugNotification("ERRO GRAVE ao ler pastas DAR! Verifique os logs.");
        } catch (...) {
            SKSE::log::critical("[ScanDarAnimations] CRASH! ERRO DESCONHECIDO E NÃO IDENTIFICADO DURANTE O SCAN!");
            RE::DebugNotification("ERRO GRAVE E DESCONHECIDO ao ler pastas DAR! Verifique os logs.");
        }

        SKSE::log::info("[ScanDarAnimations] Escaneamento finalizado. Total de {} submovesets carregados.",
                        _darSubMovesets.size());
        if (!_darSubMovesets.empty()) {
            RE::DebugNotification(std::format("{} Animações DAR carregadas.", _darSubMovesets.size()).c_str());
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

            if (ImGui::BeginChild("BibliotecaDAR", ImVec2(modal_list_size), true)) {
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
                                // O ponteiro agora aponta para um elemento no nosso vetor _darSubMovesets
                                newInstance.sourceDef = &darSubDef;
                                strcpy_s(newInstance.editedName.data(), newInstance.editedName.size(),
                                         darSubDef.name.c_str());
                                PopulateHkxFiles(newInstance);
                                _stanceToAddTo->subMovesets.push_back(newInstance);
                                SKSE::log::info("Adicionando animação DAR '{}' à stance.", darSubDef.name);
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

    // Função para dividir uma string de keywords separadas por vírgula em um vetor
    std::vector<std::string> SplitKeywords(const std::string& s) {
        std::vector<std::string> tokens;
        std::string token;
        std::istringstream tokenStream(s);
        while (std::getline(tokenStream, token, ',')) {
            // Remove espaços em branco antes e depois do token
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

        // 1. Garante que o diretório de salvamento existe
        try {
            if (!std::filesystem::exists(categoriesPath)) {
                std::filesystem::create_directories(categoriesPath);
            }
        } catch (const std::filesystem::filesystem_error& e) {
            SKSE::log::error("Falha ao criar o diretório de categorias: {}. Erro: {}", categoriesPath.string(), e.what());
            return;
        }

        SKSE::log::info("Salvando categorias customizadas em arquivos individuais...");
        std::set<std::filesystem::path> savedFilePaths;

        if (std::filesystem::exists(categoriesPath)) {
            for (const auto& entry : std::filesystem::directory_iterator(categoriesPath)) {
                if (entry.is_regular_file() && entry.path().extension() == ".json") {
                    savedFilePaths.insert(entry.path());
                }
            }
        }

        // 2. Salva cada categoria customizada em seu próprio arquivo
        for (const auto& pair : _categories) {
            const WeaponCategory& category = pair.second;
            if (category.isCustom) {
                rapidjson::Document doc;
                doc.SetObject();  // O arquivo conterá um único objeto
                auto& allocator = doc.GetAllocator();

                // Popula o objeto JSON com os dados da categoria
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
                    std::string leftHandBaseName = "Unarmed";  // Padrão
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

                // Define o caminho do arquivo e o salva
                std::filesystem::path categoryFilePath = categoriesPath / (category.name + ".json");
                std::ofstream ofs(categoryFilePath);
                if (ofs) {
                    rapidjson::StringBuffer buffer;
                    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
                    doc.Accept(writer);
                    ofs << buffer.GetString();
                    ofs.close();
                    savedFilePaths.insert(categoryFilePath);  // Adiciona à lista de arquivos válidos
                } else {
                    SKSE::log::error("Falha ao abrir {} para escrita!", categoryFilePath.string());
                }
            }
        }
        std::set<std::filesystem::path> currentCustomCategoryFiles;
        for (const auto& pair : _categories) {
            if (pair.second.isCustom) {
                currentCustomCategoryFiles.insert(categoriesPath / (pair.first + ".json"));
            }
        }
        // 3. Remove arquivos órfãos (de categorias que foram deletadas na UI)
        for (const auto& existingPath : savedFilePaths) {
            if (currentCustomCategoryFiles.find(existingPath) == currentCustomCategoryFiles.end()) {
                SKSE::log::info("Removendo arquivo de categoria órfão: {}", existingPath.string());
                std::filesystem::remove(existingPath);
            }
        }
    }

    void AnimationManager::LoadCustomCategories() {
        const std::filesystem::path categoriesPath = "Data/SKSE/Plugins/CycleMovesets/Categories";
        if (!std::filesystem::exists(categoriesPath)) {
            SKSE::log::info("Diretório de categorias customizadas não encontrado. Pulando.");
            return;
        }

        SKSE::log::info("Carregando categorias customizadas de arquivos individuais...");

        std::map<std::string, const WeaponCategory*> baseCategories;
        for (const auto& pair : _categories) {
            if (!pair.second.isCustom) {
                baseCategories[pair.second.name] = &pair.second;
            }
        }

        // Itera sobre cada arquivo no diretório de categorias
        for (const auto& entry : std::filesystem::directory_iterator(categoriesPath)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") {
                continue;
            }

            std::ifstream ifs(entry.path());
            if (!ifs) {
                SKSE::log::error("Falha ao abrir o arquivo de categoria: {}", entry.path().string());
                continue;
            }

            std::string jsonContent((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            ifs.close();

            rapidjson::Document doc;
            doc.Parse(jsonContent.c_str());

            if (doc.HasParseError() || !doc.IsObject()) {
                SKSE::log::error("Erro no parse do JSON ou o arquivo não é um objeto para: {}", entry.path().string());
                continue;
            }

            const rapidjson::Value& categoryObj = doc;  // O documento raiz é o próprio objeto

            // O resto da lógica de leitura é a mesma, mas usando 'categoryObj'
            if (!categoryObj.HasMember("name") || !categoryObj["name"].IsString() ||
                !categoryObj.HasMember("baseCategoryName") || !categoryObj["baseCategoryName"].IsString() ||
                !categoryObj.HasMember("isDualWield") || !categoryObj["isDualWield"].IsBool() ||
                !categoryObj.HasMember("keywords") || !categoryObj["keywords"].IsArray()) {
                SKSE::log::warn("Objeto de categoria customizada malformado ou com campos faltando em {}. Pulando.",
                                entry.path().string());
                continue;
            }

            std::string name = categoryObj["name"].GetString();
            std::string baseName = categoryObj["baseCategoryName"].GetString();

            auto it = baseCategories.find(baseName);
            if (it == baseCategories.end()) {
                SKSE::log::warn("Categoria base '{}' para '{}' não encontrada. Pulando.", baseName, name);
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
                newCat.isShieldCategory = false;  // Valor padrão se não existir no arquivo
            }

            for (const auto& kw : categoryObj["keywords"].GetArray()) {
                if (kw.IsString()) newCat.keywords.push_back(kw.GetString());
            }

            if (newCat.isDualWield) {
                if (!categoryObj.HasMember("leftHandBaseCategoryName") ||
                    !categoryObj["leftHandBaseCategoryName"].IsString() || !categoryObj.HasMember("leftHandKeywords") ||
                    !categoryObj["leftHandKeywords"].IsArray()) {
                    SKSE::log::warn("Categoria dual '{}' não tem campos de mão esquerda. Pulando.", name);
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

            newCat.instances.resize(1);
            newCat.stanceNames.resize(1);
            newCat.stanceNameBuffers.resize(1);


            // Agora este loop é seguro
            for (int i = 0; i < 1; ++i) {
                std::string defaultName = std::format("Stance {}", i + 1);
                newCat.stanceNames[i] = defaultName;
                strcpy_s(newCat.stanceNameBuffers[i].data(), newCat.stanceNameBuffers[i].size(), defaultName.c_str());
            }

            _categories[newCat.name] = newCat;
            _npcCategories[newCat.name] = newCat;
        }
    }

    void AnimationManager::DrawCreateCategoryModal() {
        // Determina se estamos no modo de edição baseado no ponteiro
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
            // --- Listas de categorias base para os combos ---
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

            // --- Campos do formulário ---
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

            // ============================ INÍCIO DA LÓGICA DE SALVAMENTO CORRIGIDA ============================
            const char* saveButtonText = LOC("save");
            if (ImGui::Button(saveButtonText, ImVec2(120, 0))) {
                std::string newName = _newCategoryNameBuffer;

                // Validação unificada: checa se o nome está vazio ou se já existe (considerando se é edição ou criação)
                if (newName.empty() || (!isEditing && _categories.count(newName)) ||
                    (isEditing && newName != _originalCategoryName && _categories.count(newName))) {
                    RE::DebugNotification("ERROR: Category name cannot be empty or already exists!");
                } else {
                    // --- CAMINHO DE EDIÇÃO ---
                    if (isEditing) {
                        std::string originalName = _originalCategoryName;
                        bool nameChanged = (newName != originalName);

                        if (nameChanged) {
                            // 1. Renomear arquivos de configuração
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
                                SKSE::log::error("Falha ao renomear arquivos da categoria '{}': {}", originalName,
                                                 e.what());
                            }

                            // 2. Mover o objeto em memória para a nova chave (preserva movesets e stances)
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

                        // 3. Atualizar as propriedades da categoria (que agora está no nome correto)
                        WeaponCategory& catToUpdate =
                            _categories.at(newName);  // .at() é seguro aqui pois o objeto já existe
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

                        // Sincronizar com NPCs
                        _npcCategories.at(newName) = catToUpdate;

                        // --- CAMINHO DE CRIAÇÃO (A CORREÇÃO PRINCIPAL) ---
                    } else {
                        // 1. Criar a nova categoria usando o operador []
                        WeaponCategory& newCat = _categories[newName];  // <--- CORREÇÃO: Usa [] para criar
                        newCat.name = newName;
                        newCat.isCustom = true;
                        newCat.isDualWield = _newCategoryIsDual;
                        newCat.isShieldCategory = _newCategoryIsShield;

                        // 2. Popular todas as propriedades da categoria recém-criada
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

                        newCat.instances.resize(1);
                        newCat.stanceNames.resize(1);
                        newCat.stanceNameBuffers.resize(1);
                        // 3. Inicializar os nomes padrão das stances para a nova categoria
                        for (int i = 0; i < 1; ++i) {
                            std::string defaultName = std::format("Stance {}", i + 1);
                            newCat.stanceNames[i] = defaultName;
                            strcpy_s(newCat.stanceNameBuffers[i].data(), newCat.stanceNameBuffers[i].size(),
                                     defaultName.c_str());
                        }

                        // 4. Sincronizar a nova categoria com a lista de NPCs
                        _npcCategories[newName] = newCat;
                    }

                    // Finaliza e fecha o modal
                    _categoryToEditPtr = nullptr;
                    ImGui::CloseCurrentPopup();
                }
            }
            // ============================ FIM DA LÓGICA DE SALVAMENTO CORRIGIDA ============================

            ImGui::SameLine();
            if (ImGui::Button(LOC("close"), ImVec2(120, 0))) {
                _categoryToEditPtr = nullptr;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        } else {
            // Garante que o ponteiro de edição seja limpo se o popup for fechado de outra forma
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
            _categoryToEditPtr = nullptr;  // Garante que estamos no modo de criação
            // Limpa os buffers para um formulário novo
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

            // Usamos um iterador para poder apagar da lista de forma segura
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
                        _categoryToEditPtr = &category;  // Aponta para a categoria, ativando o modo de edição

                        // Preenche os buffers com os dados atuais da categoria
                        strcpy_s(_originalCategoryName, name.c_str());  // Guarda o nome original
                        strcpy_s(_newCategoryNameBuffer, name.c_str());
                        _newCategoryIsDual = category.isDualWield;
                        _newCategoryIsShield = category.isShieldCategory;

                        // Converte vetores de keywords para strings
                        auto join_keywords = [](const std::vector<std::string>& keywords) {
                            std::string result;
                            for (size_t i = 0; i < keywords.size(); ++i) {
                                result += keywords[i] + (i == keywords.size() - 1 ? "" : ", ");
                            }
                            return result;
                        };
                        strcpy_s(_newCategoryKeywordsBuffer, join_keywords(category.keywords).c_str());
                        strcpy_s(_newCategoryLeftHandKeywordsBuffer, join_keywords(category.leftHandKeywords).c_str());

                        // --- INÍCIO DA CORREÇÃO #3: ENCONTRAR ÍNDICES CORRETOS ---
                        // Lógica para encontrar o índice da base direita
                        _newCategoryBaseIndex = 0;
                        int current_idx = 0;
                        for (const auto& pair : _categories) {
                            if (!pair.second.isCustom && !pair.second.isDualWield && !pair.second.isShieldCategory) {
                                if (pair.first == category.baseCategoryName) {
                                    _newCategoryBaseIndex = current_idx;
                                    break;
                                }
                                current_idx++;
                            }
                        }

                        // Lógica para encontrar o índice da base esquerda (se for dual wield)
                        _newCategoryLeftHandBaseIndex = 0;
                        if (category.isDualWield) {
                            current_idx = 0;
                            for (const auto& pair : _categories) {
                                if (!pair.second.isCustom) {
                                    // Precisa encontrar o nome da categoria base da mão esquerda
                                    // Esta lógica é complexa, por enquanto vamos procurar pelo typeValue
                                    if (pair.second.equippedTypeValue == category.leftHandEquippedTypeValue &&
                                        pair.second.leftHandEquippedTypeValue == category.leftHandEquippedTypeValue) {
                                        _newCategoryLeftHandBaseIndex = current_idx;
                                        break;
                                    }
                                    current_idx++;
                                }
                            }
                        }
                        // --- FIM DA CORREÇÃO #3 ---

                        _isCreateCategoryModalOpen = true;  // Abre o mesmo modal, mas agora em modo de edição
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
                SKSE::log::info("Categoria '{}' removida.", categoryToDelete);
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
        // 1. Coleta todas as keywords de categorias de escudo customizadas
        std::vector<std::string> competingKeywords;
        for (const auto& pair : _categories) {
            const WeaponCategory& otherCategory = pair.second;
            // A condição é: ser customizada, ser uma categoria de escudo, E ter keywords
            if (otherCategory.isCustom && otherCategory.isShieldCategory && !otherCategory.keywords.empty()) {
                competingKeywords.insert(competingKeywords.end(), otherCategory.keywords.begin(),
                                         otherCategory.keywords.end());
            }
        }

        if (competingKeywords.empty()) {
            return;  // Nenhuma exclusão necessária
        }

        // 2. Cria um único bloco AND para conter todas as exclusões (NOT keyword1 AND NOT keyword2 ...)
        rapidjson::Value exclusionAndBlock(rapidjson::kObjectType);
        exclusionAndBlock.AddMember("condition", "AND", allocator);
        exclusionAndBlock.AddMember("comment", "Exclude competing custom Shield + Weapon categories", allocator);
        rapidjson::Value innerExclusionConditions(rapidjson::kArrayType);

        for (const auto& keyword : competingKeywords) {
            // Adiciona a condição 'IsEquippedHasKeyword' negada para a mão direita (onde a arma com keyword está)
            AddKeywordCondition(innerExclusionConditions, keyword, false, true, allocator);
        }

        exclusionAndBlock.AddMember("Conditions", innerExclusionConditions, allocator);
        parentArray.PushBack(exclusionAndBlock, allocator);
    }

    void AnimationManager::PopulateNpcList() {
        SKSE::log::info("Iniciando escaneamento de todos os NPCs...");
        _fullNpcList.clear();
        _pluginList.clear();
        std::set<std::string> uniquePlugins;

        auto dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) {
            SKSE::log::error("Falha ao obter o TESDataHandler.");
            return;
        }

        const auto& npcArray = dataHandler->GetFormArray<RE::TESNPC>();
        for (const auto& npc : npcArray) {
            if (npc && !npc->IsPlayer() && npc->GetFile(0)) {
                NPCInfo info;
                info.formID = npc->GetFormID();

                // --- INÍCIO DA ALTERAÇÃO ---
                // Substituído npc->GetFormEditorID() pela chamada da clib_util para maior robustez.
                info.editorID = clib_util::editorID::get_editorID(npc);
                // --- FIM DA ALTERAÇÃO ---

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
        SKSE::log::info("Escaneamento concluído. {} NPCs carregados de {} plugins.", _fullNpcList.size(),
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
            // --- LOG 1: Verificar se as listas de dados principais estão carregadas ---
            if (ImGui::IsWindowAppearing()) {  // Loga apenas na primeira vez que o modal abre
                SKSE::log::info(
                    "[NpcSelectionModal] Abrindo modal. Tamanhos das listas: NPCs={}, Factions={}, Keywords={}, "
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

                // --- LOG 2: Verificar os filtros atuais ---
                //SKSE::log::info("[NpcSelectionModal] Filtros Ativos -> Texto: '{}', Plugin: '{}' (índice {})",filterTextLower, selectedPlugin, _selectedPluginIndex);

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

                        // ====================== INÍCIO DO CLIPPER MANUAL ======================
                        // 1. Pegamos a altura de uma única linha da tabela.
                        const float item_height = ImGui::GetTextLineHeightWithSpacing();

                        // 2. Pegamos a posição atual do scroll e a altura da área visível.
                        const float scroll_y = ImGui::GetScrollY();
                        ImVec2 content_avail;
                        ImGui::GetContentRegionAvail(&content_avail);
                        const float content_height = content_avail.y;

                        // 3. Calculamos o índice do primeiro e último item a serem renderizados.
                        int display_start = static_cast<int>(scroll_y / item_height);
                        int display_end = display_start + static_cast<int>(ceil(content_height / item_height)) + 1;

                        // 4. Garantimos que os índices não saiam dos limites da nossa lista.
                        display_start = std::max(0, display_start);
                        display_end = std::min(static_cast<int>(filtered_indices.size()), display_end);

                        // Log para verificar nossos cálculos manuais


                        // Adiciona espaço no topo para simular o scroll
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
                            // PASSO 1: Filtragem
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

                            // PASSO 2: Clipper Manual
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

                            // PASSO 3: Renderização
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
            if (ImGui::Button("Fechar", ImVec2(120, 0))) {
                _isNpcSelectionModalOpen = false;
            }*/
            ImGui::EndPopup();
        }
    }

    void AnimationManager::PopulateHkxFiles(CreatorSubAnimationInstance& instance) {
        if (!instance.sourceDef) return;
        try {
        // Garante que o caminho é um diretório
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
                    // Adiciona o arquivo à lista, selecionado por padrão
                    instance.hkxFileSelection[fileEntry.path().filename().string()] = true;
                }
            }
        }
        } catch (const std::filesystem::filesystem_error& e) {
            SKSE::log::critical("Erro de filesystem em PopulateHkxFiles ao ler de '{}': {}",
                                (instance.sourceDef ? instance.sourceDef->path.string() : "path_invalido"), e.what());
        }
    }

void AnimationManager::LoadGameDataForNpcRules() {
        //SKSE::log::info("Iniciando carregamento de dados (Facções, Keywords, Raças) para o Gerenciador de Regras...");

        _allFactions.clear();
        _allKeywords.clear();
        _allRaces.clear();

        auto dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) {
            SKSE::log::error("Falha ao obter o TESDataHandler. O carregamento de dados de regras foi abortado.");
            return;
        }

        // Carregar Facções
        const auto& factions = dataHandler->GetFormArray<RE::TESFaction>();
        for (const auto* faction : factions) {
            if (faction && faction->GetFile(0)) {  // Adicionada verificação de GetFile
                std::string editorID = clib_util::editorID::get_editorID(faction);
                if (!editorID.empty()) {
                    const auto* plugin = faction->GetFile(0);
                    _allFactions.push_back({faction->GetFormID(), editorID, std::string(plugin->GetFilename())});
                }
            }
        }
        SKSE::log::info("Carregadas {} facções.", _allFactions.size());

        // Carregar Keywords
        const auto& keywords = dataHandler->GetFormArray<RE::BGSKeyword>();
        for (const auto* keyword : keywords) {
            if (keyword && keyword->GetFile(0)) {  // Adicionada verificação de GetFile
                std::string editorID = clib_util::editorID::get_editorID(keyword);
                if (!editorID.empty()) {
                    const auto* plugin = keyword->GetFile(0);
                    _allKeywords.push_back({keyword->GetFormID(), editorID, std::string(plugin->GetFilename())});
                }
            }
        }
        SKSE::log::info("Carregadas {} keywords.", _allKeywords.size());

        // Carregar Raças
        const auto& races = dataHandler->GetFormArray<RE::TESRace>();
        for (const auto* race : races) {
            if (race && race->GetFile(0)) {  // Adicionada verificação de GetFile
                std::string editorID = clib_util::editorID::get_editorID(race);
                if (!editorID.empty() && editorID != "PlayerRace") {
                    const auto* plugin = race->GetFile(0);
                    _allRaces.push_back(
                        {race->GetFormID(), editorID, race->GetFullName(), std::string(plugin->GetFilename())});
                }
            }
        }
        SKSE::log::info("Carregadas {} raças (não 'PlayerRace').", _allRaces.size());
    }



    // Formata o FormID para o formato de 6 dígitos que o OAR espera, removendo o índice do plugin
    std::string FormatFormIDForOAR(RE::FormID formID, const std::string& pluginName) {
        auto dataHandler = RE::TESDataHandler::GetSingleton();
        if (dataHandler) {
            const RE::TESFile* plugin = dataHandler->LookupModByName(pluginName);

            // Verifica se o plugin foi encontrado e se ele tem a flag "IsLight" (ESL)
            if (plugin && plugin->IsLight()) {
                // Para plugins ESL, o ID real está nos últimos 12 bits (3 dígitos hex)
                return std::format("{:03X}", formID & 0xFFF);
            }
        }
        // Para plugins normais (ESM/ESP), o ID está nos últimos 24 bits (6 dígitos hex)
        return std::format("{:06X}", formID & 0xFFFFFF);
    }

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
        params.AddMember("formID", rapidjson::Value(FormatFormIDForOAR(formID, plugin).c_str(), allocator), allocator);
        condition.AddMember("Actor base", params, allocator);
        conditionsArray.PushBack(condition, allocator);
    }

    void AnimationManager::AddIsInFactionCondition(rapidjson::Value& conditionsArray, const std::string& plugin,
                                                   RE::FormID formID, rapidjson::Document::AllocatorType& allocator) {
        rapidjson::Value condition(rapidjson::kObjectType);
        condition.AddMember("condition", "IsInFaction", allocator);
        rapidjson::Value params(rapidjson::kObjectType);
        params.AddMember("pluginName", rapidjson::Value(plugin.c_str(), allocator), allocator);
        params.AddMember("formID", rapidjson::Value(FormatFormIDForOAR(formID, plugin).c_str(), allocator), allocator);
        condition.AddMember("Faction", params, allocator);
        conditionsArray.PushBack(condition, allocator);
    }

    void AnimationManager::AddHasKeywordCondition(rapidjson::Value& conditionsArray, const std::string& plugin,
                                                  RE::FormID formID, rapidjson::Document::AllocatorType& allocator) {
        rapidjson::Value condition(rapidjson::kObjectType);
        condition.AddMember("condition", "HasKeyword", allocator);
        condition.AddMember("requiredVersion", "1.0.0.0", allocator);

        // Objeto principal que será o valor da chave "Keyword"
        rapidjson::Value keywordObj(rapidjson::kObjectType);

        // Objeto interno "form"
        rapidjson::Value formObj(rapidjson::kObjectType);
        formObj.AddMember("pluginName", rapidjson::Value(plugin.c_str(), allocator), allocator);
        formObj.AddMember("formID", rapidjson::Value(FormatFormIDForOAR(formID, plugin).c_str(), allocator), allocator);

        // Adiciona o objeto "form" dentro do objeto "Keyword"
        keywordObj.AddMember("form", formObj, allocator);

        // Adiciona o objeto "Keyword" completo à condição principal
        condition.AddMember("Keyword", keywordObj, allocator);

        conditionsArray.PushBack(condition, allocator);
    }

    void AnimationManager::AddIsRaceCondition(rapidjson::Value& conditionsArray, const std::string& plugin,
                                              RE::FormID formID, rapidjson::Document::AllocatorType& allocator) {
        rapidjson::Value condition(rapidjson::kObjectType);
        condition.AddMember("condition", "IsRace", allocator);
        rapidjson::Value params(rapidjson::kObjectType);
        params.AddMember("pluginName", rapidjson::Value(plugin.c_str(), allocator), allocator);
        params.AddMember("formID", rapidjson::Value(FormatFormIDForOAR(formID, plugin).c_str(), allocator), allocator);
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
            // Retorna a regra geral como padrão, mesmo que vazia
            SKSE::log::info("[FindBestMoveset] Ator nulo fornecido. Retornando regra geral padrão.");
            return {&_generalNpcRule, 0, GetPriorityForType(RuleType::GeneralNPC)};
        }
        //SKSE::log::info("=====================================================================");
        //SKSE::log::info("[FindBestMoveset] Inciando busca para o ator: '{}' ({:08X}), Categoria: '{}'",actor->GetName(), actor->GetFormID(), categoryName);
    
        const std::vector<RuleType> priorityOrder = {RuleType::UniqueNPC, RuleType::Keyword, RuleType::Faction,
                                                     RuleType::Race};

        // Itera pela ordem de prioridade dos TIPOS de regra
        for (const auto& typeToFind : priorityOrder) {
            //SKSE::log::info("[FindBestMoveset] Checando regras do tipo: {}", RuleTypeToString(typeToFind));
            // Itera pela lista de regras da UI (respeitando a sub-prioridade da ordem da lista)
            for (const auto& rule : _npcRules) {
                if (rule.type != typeToFind) continue;

                // Verifica se a regra se aplica ao ator
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
                        //SKSE::log::info("    [MATCH!] A regra '{}' se aplica ao ator.", rule.displayName);
                        const auto& category = category_it->second;
                        int count = 0;
                        // Calcula a contagem de movesets selecionados (ainda útil ter essa info)
                        for (const auto& modInst : category.instances[0].modInstances) {
                            if (modInst.isSelected) count++;
                        }

                        if (count > 0) {
                            //SKSE::log::info("    -> Categoria tem {} movesets. RETORNANDO ESTA REGRA.", count);
                            // AQUI ESTÁ A MUDANÇA: Retornamos um ponteiro para a regra atual (&rule)
                            return {&rule, count, GetPriorityForType(rule.type)};
                        }
                    }
                }
            }
        }

        // Se nenhuma regra específica foi encontrada, usa a regra Geral como fallback
        auto category_it = _generalNpcRule.categories.find(categoryName);
        if (category_it != _generalNpcRule.categories.end()) {
            int count = 0;
            for (const auto& modInst : category_it->second.instances[0].modInstances) {
                if (modInst.isSelected) count++;
            }
            // Retorna o ponteiro para a regra geral
            return {&_generalNpcRule, count, GetPriorityForType(RuleType::GeneralNPC)};
        }

        // Fallback final: retorna a regra geral mesmo que não tenha a categoria
        return {&_generalNpcRule, 0, GetPriorityForType(RuleType::GeneralNPC)};
    }

    std::vector<GlobalControl::MovesetCandidate> AnimationManager::GetAvailableMovesetIndices(RE::Actor* actor,const std::string& categoryName) {
        if (!actor) return {};
        //SKSE::log::info("---------------------------------------------------------------------");
        //SKSE::log::info("[GetAvailableIndices] Buscando índices para o ator: '{}', Categoria: '{}'", actor->GetName(),categoryName);
        std::vector<GlobalControl::MovesetCandidate> candidates;

        // Obtém os status do ator uma única vez
        float currentHealth = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
        float maxHealth = actor->GetActorValueMax(RE::ActorValue::kHealth);
        float hpPercent = (maxHealth > 0) ? (currentHealth / maxHealth) * 100.0f : 0.0f;
        int level = actor->GetLevel();
        float currentStamina = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
        float maxStamina = actor->GetActorValueMax(RE::ActorValue::kStamina);
        float stPercent = (maxStamina > 0) ? (currentStamina / maxStamina) * 100.0f : 0.0f;
        float currentMagicka = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kMagicka);
        float maxMagicka = actor->GetActorValueMax(RE::ActorValue::kMagicka);
        float mkPercent = (maxMagicka > 0) ? (currentMagicka / maxMagicka) * 100.0f : 0.0f;

        // Função interna para processar uma regra e adicionar seus movesets à lista de candidatos
        auto processRule = [&](const MovesetRule* rule) {
            if (!rule) return;

            auto categoryIt = rule->categories.find(categoryName);
            if (categoryIt == rule->categories.end()) {
                return;
            }

            const CategoryInstance& instance = categoryIt->second.instances[0];
            int currentPriority = GetPriorityForType(rule->type);
            int currentPlaylistIndex = 1;

            for (const auto& modInst : instance.modInstances) {
                if (modInst.isSelected) {
                    bool allPerksMet = true;
                    std::vector<PerkDef> perksToCheck;
                    perksToCheck.insert(perksToCheck.end(), instance.perkList.begin(), instance.perkList.end());
                    perksToCheck.insert(perksToCheck.end(), modInst.perkList.begin(), modInst.perkList.end());
                    // Não precisamos dos perks de sub-moveset aqui, pois estamos avaliando o moveset como um todo.

                    if (!perksToCheck.empty()) {
                        for (const auto& perkDef : perksToCheck) {
                            const auto* requiredPerk = RE::TESForm::LookupByID<RE::BGSPerk>(perkDef.formID);
                            if (!requiredPerk || !actor->HasPerk(const_cast<RE::BGSPerk*>(requiredPerk))) {
                                allPerksMet = false;
                                break;
                            }
                        }
                    }

                    // 2. Se os perks não forem atendidos, pula para o próximo moveset.
                    if (!allPerksMet) {
                        continue;
                    }

                    bool conditionsMet = (hpPercent <= modInst.hp && level >= modInst.level &&
                                          stPercent <= modInst.st && mkPercent <= modInst.mn);
                    if (conditionsMet) {
                        float hp_distance = modInst.hp - hpPercent;
                        float level_distance = level - modInst.level;
                        float st_distance = modInst.st - stPercent;
                        float mn_distance = modInst.mn - mkPercent;
                        float totalScore = hp_distance + level_distance + st_distance + mn_distance;

                        // AQUI ESTÁ A MÁGICA: Adicionamos o candidato com seu índice e prioridade originais!
                        candidates.push_back({currentPlaylistIndex, currentPriority, totalScore});
                    }
                }
                currentPlaylistIndex++;
            }
        };

        if (Settings::EnableAllNPC) {
            //SKSE::log::info("[GetAvailableIndices] Modo 'EnableAllNPC' ativo. Combinando todas as regras aplicáveis.");

            std::vector<ModInstance> combinedModInstances;
            int highestPriority = -1;  // Começa com -1 para garantir que a prioridade 0 (General) seja pega corretamente

            // 1. Itera por todas as regras específicas para encontrar matches
            for (const auto& rule : _npcRules) {
                bool match = false;
                switch (rule.type) {
                    case RuleType::UniqueNPC:
                        if (actor->GetActorBase()->GetFormID() == rule.formID) match = true;
                        break;
                    case RuleType::Keyword:
                        if (actor->GetActorBase()->HasKeywordString(rule.identifier)) match = true;
                        break;
                    case RuleType::Faction:
                        if (actor->GetActorBase()->IsInFaction(
                                RE::TESForm::LookupByEditorID<RE::TESFaction>(rule.identifier)))
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
                    processRule(&rule);
                }
            }
            processRule(&_generalNpcRule);
        }  else {  
            NpcRuleMatch match = FindBestMovesetConfiguration(actor, categoryName);
            processRule(match.rule);
        }
        std::sort(candidates.begin(), candidates.end());
        return candidates;
    }

    void Settings::SyncMovementKeys() {
        keyForward = static_cast<uint32_t>(Settings::keyForward_k);
        keyBack = static_cast<uint32_t>(Settings::keyBack_k);
        keyLeft = static_cast<uint32_t>(Settings::keyLeft_k);
        keyRight = static_cast<uint32_t>(Settings::keyRight_k);
        SKSE::log::info("Teclas de movimento sincronizadas para o runtime.");
    }

    std::optional<std::pair<size_t, size_t>> AnimationManager::FindSubAnimationByPath(
        const std::filesystem::path& configPath) {
        for (size_t modIdx = 0; modIdx < _allMods.size(); ++modIdx) {
            const auto& mod = _allMods[modIdx];
            for (size_t subIdx = 0; subIdx < mod.subAnimations.size(); ++subIdx) {
                try {
                    // A comparação com equivalent agora deve ser segura
                    if (std::filesystem::exists(mod.subAnimations[subIdx].path) &&
                        std::filesystem::exists(configPath) &&
                        std::filesystem::equivalent(mod.subAnimations[subIdx].path, configPath)) {
                        return std::make_pair(modIdx, subIdx);
                    }
                } catch (const std::filesystem::filesystem_error& e) {
                    SKSE::log::error("Erro de Filesystem ao comparar caminhos: '{}' e '{}'. Erro: {}",
                                     PathToUTF8(mod.subAnimations[subIdx].path), PathToUTF8(configPath), e.what());
                    // Continua a procurar em vez de crashar
                }
            }
        }
        return std::nullopt;
    }

    void AnimationManager::AddFullCategoryConditions(rapidjson::Value& parentArray, const WeaponCategory& category,
                                                     rapidjson::Document::AllocatorType& allocator) {
        // Condição de Tipo de Equipamento (Mão Direita)
        AddCompareEquippedTypeCondition(parentArray, category.equippedTypeValue, false, allocator);

        // Condições de Keyword (Mão Direita) - Inclusão e Exclusão
        AddKeywordOrConditions(parentArray, category.keywords, false, allocator);
        AddCompetingKeywordExclusions(parentArray, &category, false, allocator);

        // Condições para Mão Esquerda (se aplicável)
        if (category.leftHandEquippedTypeValue >= 0.0) {
            AddCompareEquippedTypeCondition(parentArray, category.leftHandEquippedTypeValue, true, allocator);
        }

        // Condições de Keyword para Mão Esquerda (se aplicável)
        if (!category.leftHandKeywords.empty()) {
            AddKeywordOrConditions(parentArray, category.leftHandKeywords, true, allocator);
            AddCompetingKeywordExclusions(parentArray, &category, true, allocator);
        }

        // Lógica especial para a categoria base "Shield" para excluir customizadas
        if (category.isShieldCategory && !category.isCustom) {
            AddShieldCategoryExclusions(parentArray, allocator);
        }
    }

    void AnimationManager::ConvertAllMcoToBfco() {
        SKSE::log::info("Iniciando solicitação de conversão global MCO para BFCO via CycleDar.json...");
        int filesCreatedOrUpdated = 0;

        // Itera por toda a biblioteca de animações carregada (_allMods)
        for (const auto& modDef : _allMods) {
            for (const auto& subAnimDef : modDef.subAnimations) {
                // 1. Encontra o caminho da pasta da sub-animação
                std::filesystem::path folderPath;
                if (subAnimDef.path.filename() == "config.json") {
                    folderPath = subAnimDef.path.parent_path();
                } else {
                    folderPath = subAnimDef.path;  // Para DAR, o path já é a pasta
                }

                // 2. Define o caminho do CycleDar.json
                std::filesystem::path cycleDarPath = folderPath / "CycleDar.json";

                rapidjson::Document doc;

                // 3. Tenta ler o arquivo existente
                std::ifstream fileStream(cycleDarPath);
                if (fileStream) {
                    std::string jsonContent((std::istreambuf_iterator<char>(fileStream)),
                                            std::istreambuf_iterator<char>());
                    fileStream.close();
                    if (doc.Parse(jsonContent.c_str()).HasParseError()) {
                        SKSE::log::error("Erro de Parse ao ler {}. Criando um novo arquivo.", cycleDarPath.string());
                        doc.SetObject();
                    }
                } else {
                    doc.SetObject();
                }
                if (!doc.IsObject()) doc.SetObject();

                auto& allocator = doc.GetAllocator();
                if (!doc.HasMember("sources")) {
                    doc.AddMember("sources", rapidjson::Value(rapidjson::kArrayType), allocator);
                } else if (!doc["sources"].IsArray()) {
                    // Se existir mas não for array, substitui por um array vazio
                    doc["sources"].SetArray();
                }
                if (doc.HasMember("convertBFCO")) {
                    doc["convertBFCO"].SetBool(true);
                } else {
                    doc.AddMember("convertBFCO", true, allocator);
                }

                // 5. Salva o arquivo JSON
                FILE* fp;
                fopen_s(&fp, cycleDarPath.string().c_str(), "wb");
                if (!fp) {
                    SKSE::log::error("Falha ao abrir o arquivo para escrita: {}", cycleDarPath.string());
                    continue;
                }
                char writeBuffer[65536];
                rapidjson::FileWriteStream os(fp, writeBuffer, sizeof(writeBuffer));
                rapidjson::PrettyWriter<rapidjson::FileWriteStream> writer(os);
                doc.Accept(writer);
                fclose(fp);

                filesCreatedOrUpdated++;
            }
        }

        SKSE::log::info("Solicitação de conversão concluída. {} arquivos CycleDar.json foram criados/atualizados.",
                        filesCreatedOrUpdated);
        RE::DebugNotification(
            std::format("BFCO conversion requested for {} movesets. Reload game.", filesCreatedOrUpdated).c_str());

        // Mostra o pop-up de reinício, pois a conversão real só ocorrerá no próximo load
        _showRestartPopup = true;
    }

    std::optional<int64_t> AnimationManager::GetFileTime(const std::filesystem::path& path) {
        try {
            if (std::filesystem::exists(path)) {
                auto ftime = std::filesystem::last_write_time(path);
                return ftime.time_since_epoch().count();
            }
        } catch (const std::filesystem::filesystem_error& e) {
            SKSE::log::error("Erro ao acessar o tempo do arquivo {}: {}", path.string(), e.what());
        }
        return std::nullopt;
    }
    std::map<std::string, int64_t> AnimationManager::GetCurrentAnimationState() {
        std::map<std::string, int64_t> currentState;
        const std::vector<std::filesystem::path> rootPaths = {
            "Data/meshes/actors/character/animations/OpenAnimationReplacer",
            "Data/meshes/actors/character/animations/DynamicAnimationReplacer/_CustomConditions"};

        for (const auto& rootPath : rootPaths) {
            if (!std::filesystem::exists(rootPath) || !std::filesystem::is_directory(rootPath)) {
                continue;
            }

            try {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(rootPath)) {
                    if (entry.is_directory()) {
                        const auto& dirPath = entry.path();

                        bool hasHkx = false;
                        for (const auto& file : std::filesystem::directory_iterator(dirPath)) {
                            if (file.is_regular_file() && file.path().extension() == ".hkx") {
                                hasHkx = true;
                                break;
                            }
                        }

                        bool isValidModFolder = hasHkx && (std::filesystem::exists(dirPath / "_conditions.txt") ||
                                                           std::filesystem::exists(dirPath / "config.json") ||
                                                           std::filesystem::exists(dirPath / "user.json"));

                        if (isValidModFolder) {
                            auto modTimeOpt = GetFileTime(dirPath);
                            if (modTimeOpt) {
                                currentState[PathToUTF8(dirPath)] = *modTimeOpt;
                            }
                        }
                    }
                }
            } catch (const std::filesystem::filesystem_error& e) {
                SKSE::log::error("Erro de filesystem em GetCurrentAnimationState ao escanear '{}': {}",
                                 PathToUTF8(rootPath), e.what());
            }
        }
        return currentState;
    }
    // Converte um objeto JSON para a sua struct SubAnimationDef
    void AnimationManager::FromJson(const rapidjson::Value& json, SubAnimationDef& subAnimDef) {
        subAnimDef.name = json["name"].GetString();
        subAnimDef.path = std::filesystem::u8path(json["path"].GetString());
        subAnimDef.attackCount = json["attackCount"].GetInt();
        subAnimDef.powerAttackCount = json["powerAttackCount"].GetInt();
        subAnimDef.hasIdle = json["hasIdle"].GetBool();
        subAnimDef.hasAnimations = json["hasAnimations"].GetBool();
        subAnimDef.hasCPA = json["hasCPA"].GetBool();
        subAnimDef.dpaTags.hasA = json["dpaTags"]["A"].GetBool();
        subAnimDef.dpaTags.hasB = json["dpaTags"]["B"].GetBool();
        subAnimDef.dpaTags.hasL = json["dpaTags"]["L"].GetBool();
        subAnimDef.dpaTags.hasR = json["dpaTags"]["R"].GetBool();
    }

    // Converte sua struct SubAnimationDef para um objeto JSON
    void AnimationManager::ToJson(rapidjson::PrettyWriter<rapidjson::StringBuffer>& writer,
                                  const SubAnimationDef& subAnimDef) {
        writer.StartObject();
        writer.Key("name");
        writer.String(subAnimDef.name.c_str());
        writer.Key("path");
        writer.String(PathToUTF8(subAnimDef.path).c_str());
        writer.Key("attackCount");
        writer.Int(subAnimDef.attackCount);
        writer.Key("powerAttackCount");
        writer.Int(subAnimDef.powerAttackCount);
        writer.Key("hasIdle");
        writer.Bool(subAnimDef.hasIdle);
        writer.Key("hasAnimations");
        writer.Bool(subAnimDef.hasAnimations);
        writer.Key("hasCPA");
        writer.Bool(subAnimDef.hasCPA);
        writer.Key("dpaTags");
        writer.StartObject();
        writer.Key("A");
        writer.Bool(subAnimDef.dpaTags.hasA);
        writer.Key("B");
        writer.Bool(subAnimDef.dpaTags.hasB);
        writer.Key("L");
        writer.Bool(subAnimDef.dpaTags.hasL);
        writer.Key("R");
        writer.Bool(subAnimDef.dpaTags.hasR);
        writer.EndObject();
        writer.EndObject();
    }

    // Converte um objeto JSON para a sua struct AnimationModDef
    void AnimationManager::FromJson(const rapidjson::Value& json, AnimationModDef& modDef) {
        modDef.name = json["name"].GetString();
        modDef.author = json["author"].GetString();
        const rapidjson::Value& subAnims = json["subAnimations"];
        for (const auto& subJson : subAnims.GetArray()) {
            SubAnimationDef subDef;
            FromJson(subJson, subDef);
            modDef.subAnimations.push_back(subDef);
        }
    }

    // Converte sua struct AnimationModDef para um objeto JSON
    void AnimationManager::ToJson(rapidjson::PrettyWriter<rapidjson::StringBuffer>& writer,
                                  const AnimationModDef& modDef) {
        writer.StartObject();
        writer.Key("name");
        writer.String(modDef.name.c_str());
        writer.Key("author");
        writer.String(modDef.author.c_str());
        writer.Key("subAnimations");
        writer.StartArray();
        for (const auto& subAnim : modDef.subAnimations) {
            ToJson(writer, subAnim);
        }
        writer.EndArray();
        writer.EndObject();
    }

    void AnimationManager::LoadAnimationLibrary() {
        _cacheWasInvalid = false;
        const std::filesystem::path cachePath = "Data/SKSE/Plugins/CycleMovesets/CMF_Cache.json";

        // Tenta validar o cache. Se for válido, carrega a biblioteca a partir dele.
        if (ValidateCache(cachePath)) {
            SKSE::log::info("Cache da biblioteca de animações é válido. Carregando do cache...");

            std::ifstream ifs(cachePath);
            if (!ifs) {
                SKSE::log::error(
                    "Falha ao abrir o arquivo de cache para leitura, mesmo após validação. Forçando re-scan.");
                _cacheWasInvalid = true;
                PerformFullScanAndSaveCache();
                return;
            }
            std::string jsonContent((std::istreambuf_iterator<char>(ifs)), {});
            ifs.close();

            rapidjson::Document doc;
            doc.Parse(jsonContent.c_str());

            if (doc.HasParseError() || !doc.HasMember("library") || !doc["library"].IsArray()) {
                SKSE::log::error("Arquivo de cache corrompido. Forçando re-scan.");
                _cacheWasInvalid = true;
                PerformFullScanAndSaveCache();
                return;
            }

            _allMods.clear();
            const rapidjson::Value& library = doc["library"];
            for (const auto& modJson : library.GetArray()) {
                AnimationModDef modDef;
                FromJson(modJson, modDef);
                _allMods.push_back(modDef);
            }
            SKSE::log::info("Biblioteca de animações carregada do cache com sucesso. {} mods carregados.",
                            _allMods.size());
            return;  // Caminho rápido concluído!
        }

        // Se o cache não for válido ou não existir, executa o escaneamento completo.
        _cacheWasInvalid = true;
        SaveAllSettings();
        PerformFullScanAndSaveCache();
    }

    // A função que valida o manifesto do cache contra o estado do disco.
    bool AnimationManager::ValidateCache(const std::filesystem::path& cachePath) {
        if (!std::filesystem::exists(cachePath)) {
            SKSE::log::info("Arquivo de cache não encontrado.");
            return false;
        }

        std::ifstream ifs(cachePath);
        if (!ifs) {
            SKSE::log::warn("Não foi possível abrir o arquivo de cache para leitura.");
            return false;
        }
        std::string jsonContent((std::istreambuf_iterator<char>(ifs)), {});
        ifs.close();

        rapidjson::Document doc;
        doc.Parse(jsonContent.c_str());

        if (doc.HasParseError() || !doc.HasMember("manifest") || !doc["manifest"].IsArray()) {
            SKSE::log::warn("Cache corrompido (JSON inválido ou sem manifesto).");
            return false;  // Cache corrompido
        }

        std::map<std::string, int64_t> manifestMap;
        const rapidjson::Value& manifestArray = doc["manifest"];
        for (const auto& entry : manifestArray.GetArray()) {
            if (entry.IsObject() && entry.HasMember("path") && entry.HasMember("last_modified")) {
                manifestMap[entry["path"].GetString()] = entry["last_modified"].GetInt64();
            }
        }

        // Pega o estado atual REAL do disco
        std::map<std::string, int64_t> currentState = GetCurrentAnimationState();

        // 1. Verificação de Tamanho: A maneira mais rápida de detectar adições/remoções.
        if (manifestMap.size() != currentState.size()) {
            SKSE::log::info("Cache inválido: Número de pastas de animação mudou (Manifesto: {}, Disco: {}).",
                            manifestMap.size(), currentState.size());
            return false;
        }

        // 2. Verificação de Conteúdo e Timestamps
        for (const auto& [path, timestamp] : currentState) {
            auto it = manifestMap.find(path);
            if (it == manifestMap.end()) {
                SKSE::log::info("Cache inválido: Nova pasta de animação detectada em '{}'.", path);
                return false;  // Pasta nova encontrada que não estava no manifesto.
            }
            if (it->second != timestamp) {
                SKSE::log::info("Cache inválido: Modificação detectada em '{}'.", path);
                return false;  // Timestamp da pasta mudou.
            }
        }

        SKSE::log::info("Cache validado com sucesso.");
        return true;  // Se todas as verificações passaram, o cache é válido!
    }
    // Sua lógica de escaneamento original, agora refatorada para também construir e salvar o cache.
    void AnimationManager::PerformFullScanAndSaveCache() {
        _allMods.clear();
        _darSubMovesets.clear();

        // --- Escaneamento OAR ---
        auto processModDirectory = [&](const std::filesystem::path& rootPath) {
            if (!std::filesystem::exists(rootPath)) return;
            // Itera para encontrar pastas que são a raiz de um "mod" (contêm config.json)
            for (const auto& entry : std::filesystem::recursive_directory_iterator(rootPath)) {
                if (entry.is_directory() && std::filesystem::exists(entry.path() / "config.json")) {
                    // Verificamos se a pasta pai também tem um config.json. Se tiver, esta é uma sub-animação, não um
                    // mod raiz.
                    std::filesystem::path parentPath = entry.path().parent_path();
                    if (parentPath != rootPath && std::filesystem::exists(parentPath / "config.json")) {
                        continue;  // Pula sub-animações, ProcessTopLevelMod cuidará delas
                    }
                    ProcessTopLevelMod(entry.path());
                }
            }
        };

        const std::filesystem::path oarRootPath = "Data\\meshes\\actors\\character\\animations\\OpenAnimationReplacer";
        processModDirectory(oarRootPath);

        // --- Escaneamento DAR ---
        ScanDarAnimations();  // Sua função original que popula _darSubMovesets
        if (!_darSubMovesets.empty()) {
            AnimationModDef darModDef;
            darModDef.name = "[DAR] Animations";
            darModDef.author = "Dynamic Animation Replacer";
            darModDef.subAnimations = _darSubMovesets;
            _allMods.push_back(darModDef);
        }

        SKSE::log::info("Escaneamento completo finalizado. {} mods carregados.", _allMods.size());

        // --- Construção e Salvamento do Manifesto ---
        // A fonte da verdade agora é a função que lê o disco diretamente.
        std::map<std::string, int64_t> newManifestData = GetCurrentAnimationState();
        SaveAnimationLibraryCache(newManifestData);
    }

    // Salva o estado atual da biblioteca e o manifesto no arquivo de cache.
    void AnimationManager::SaveAnimationLibraryCache(const std::map<std::string, int64_t>& manifest) {
        const std::filesystem::path cachePath = "Data/SKSE/Plugins/CycleMovesets/CMF_Cache.json";
        try {
            if (!std::filesystem::exists(cachePath.parent_path())) {
                std::filesystem::create_directories(cachePath.parent_path());
                SKSE::log::info("Diretório de cache criado em: {}", PathToUTF8(cachePath.parent_path()));
            }
        } catch (const std::filesystem::filesystem_error& e) {
            SKSE::log::error("Falha ao criar o diretório de cache: {}", e.what());
            return;
        }

        rapidjson::StringBuffer sb;
        rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(sb);

        writer.StartObject();

        // Salva o Manifesto
        writer.Key("manifest");
        writer.StartArray();
        for (const auto& [path, timestamp] : manifest) {
            writer.StartObject();
            writer.Key("path");
            writer.String(path.c_str());
            writer.Key("last_modified");
            writer.Int64(timestamp);
            writer.EndObject();
        }
        writer.EndArray();

        // Salva a Biblioteca de Animações
        writer.Key("library");
        writer.StartArray();
        for (const auto& modDef : _allMods) {
            ToJson(writer, modDef);  // Sua função ToJson existente
        }
        writer.EndArray();

        writer.EndObject();

        // Escreve o arquivo
        std::ofstream ofs(cachePath);
        if (ofs) {
            ofs << sb.GetString();
            ofs.close();
            SKSE::log::info("Novo cache da biblioteca de animações salvo em '{}'.", PathToUTF8(cachePath));
        } else {
            SKSE::log::error("Falha ao salvar o cache da biblioteca em '{}'.", PathToUTF8(cachePath));
        }
    }

    void AnimationManager::AddHasPerkCondition(rapidjson::Value& conditionsArray, const std::string& plugin,
                                               RE::FormID formID, rapidjson::Document::AllocatorType& allocator) {
        if (plugin.empty() || formID == 0) {
            return;  // Não adiciona a condição se o perk não for válido
        }

        rapidjson::Value condition(rapidjson::kObjectType);
        condition.AddMember("condition", "HasPerk", allocator);

        rapidjson::Value params(rapidjson::kObjectType);
        params.AddMember("pluginName", rapidjson::Value(plugin.c_str(), allocator), allocator);
        params.AddMember("formID", rapidjson::Value(FormatFormIDForOAR(formID, plugin).c_str(), allocator), allocator);

        condition.AddMember("Perk", params, allocator);
        conditionsArray.PushBack(condition, allocator);
    }

    void AnimationManager::AddIsEquipSlotOccupiedCondition(rapidjson::Value& conditionsArray,
                                                           const std::string& slotName, bool negated,
                                                           rapidjson::Document::AllocatorType& allocator) {
        rapidjson::Value condition(rapidjson::kObjectType);
        condition.AddMember("condition", "IsEquipSlotOccupied", allocator);
        // Usando o nome do plugin do seu exemplo
        condition.AddMember("requiredPlugin", "CycleMovesets", allocator);
        condition.AddMember("requiredVersion", "1.0.0.0", allocator);

        if (negated) {
            condition.AddMember("negated", true, allocator);
        }

        condition.AddMember("Slot Name", rapidjson::Value(slotName.c_str(), allocator), allocator);
        conditionsArray.PushBack(condition, allocator);
    }




RE::InventoryEntryData* Hooks::GetSelectedEntryInMenu() {
        // Pega a interface de usuário (UI) do jogo
        if (const auto ui = RE::UI::GetSingleton()) {
            // 1. VERIFICA SE O MENU DO CONTÊINER ESTÁ ABERTO
            if (const auto menu_c = ui->GetMenu<RE::ContainerMenu>()) {
                if (const auto a_itemList = menu_c->GetRuntimeData().itemList) {
                    if (const auto item = a_itemList->GetSelectedItem()) {
                        // Retorna os dados do item selecionado
                        return item->data.objDesc;
                    }
                }
            }
            // 2. SE NÃO, VERIFICA SE O MENU DO INVENTÁRIO DO JOGADOR ESTÁ ABERTO
            else if (const auto menu_i = ui->GetMenu<RE::InventoryMenu>()) {
                if (const auto a_itemList = menu_i->GetRuntimeData().itemList) {
                    if (const auto item = a_itemList->GetSelectedItem()) {
                        // Retorna os dados do item selecionado
                        return item->data.objDesc;
                    }
                }
            }
            // 3. SE NÃO, VERIFICA SE O MENU DE FAVORITOS ESTÁ ABERTO (lógica um pouco diferente)
            else if (const auto menu_f = ui->GetMenu<RE::FavoritesMenu>()) {
                RE::GFxValue selectedIndex;
                const auto& runtime_data = menu_f->GetRuntimeData();
                if (runtime_data.root.GetMember("selectedIndex", &selectedIndex) && selectedIndex.IsNumber()) {
                    const std::int32_t selected_index = static_cast<std::int32_t>(selectedIndex.GetNumber());
                    const auto& items = runtime_data.favorites;
                    if (selected_index >= 0 && static_cast<uint32_t>(selected_index) < items.size()) {
                        // Retorna os dados do item selecionado no menu de favoritos
                        return items[selected_index].entryData;
                    }
                }
            }
        }

        // Se nenhum menu relevante estiver aberto ou nenhum item estiver selecionado, retorna nulo
        return nullptr;
    }

    void Hooks::Install() {

        auto& trampoline = SKSE::GetTrampoline();
        constexpr size_t size_per_hook = 14;
        constexpr size_t NUM_TRAMPOLINE_HOOKS = 3;
        trampoline.create(size_per_hook * NUM_TRAMPOLINE_HOOKS);

        const REL::Relocation<std::uintptr_t> function{REL::RelocationID(51019, 51897)};
        InventoryHoverHook::originalFunction =
            trampoline.write_call<5>(function.address() + REL::Relocate(0x114, 0x22c), InventoryHoverHook::thunk);
        const REL::Relocation<std::uintptr_t> target{REL::RelocationID(37938, 38894)};
        GlobalControl::Equip2H::func =
            trampoline.write_call<5>(target.address() + REL::Relocate(0xe5, 0x170), GlobalControl::Equip2H::thunk);
        const REL::Relocation<std::uintptr_t> targetU{REL::RelocationID(37945, 38901)};
        GlobalControl::Unequip2H::func =
            trampoline.write_call<5>(targetU.address() + REL::Relocate(0x138, 0x1b9), GlobalControl::Unequip2H::thunk);
    }

int64_t Hooks::InventoryHoverHook::thunk(RE::InventoryEntryData* a1) {
        const auto ui = RE::UI::GetSingleton();

        // Verificar se a UI existe E se o "InventoryMenu" está aberto
        if (ui && ui->IsMenuOpen("InventoryMenu")) {
        if (is_open.load()) {
    #ifdef GetObject
    #undef GetObject
    #endif
            if (const auto a_bound = a1->GetObject()) {
                if (a_bound->IsWeapon()) {
                    //logger::info("Mostrando prompt de arma para {}", a_bound->GetName());
                    GlobalControl::EquipMenu::GetSingleton()->Show(a_bound);
                } else {
                    // Em vez de 'RemovePrompt', chamamos nossa função 'Hide'
                    GlobalControl::EquipMenu::GetSingleton()->Hide();
                }
            } else {
                // Se não houver objeto, garantimos que os prompts estão escondidos
                GlobalControl::EquipMenu::GetSingleton()->Hide();
            }
        }
        } else {
            GlobalControl::EquipMenu::GetSingleton()->Hide();
        }

        return originalFunction(a1);
}

void AnimationManager::DrawConditionsEffectsPopup() {
    if (_isConditionsEffectsPopupOpen &&
        !_hitRuleToEdit) {  // Só define o proprietário se não estivermos editando uma hit rule
        if (_stanceToEditPerk || _stanceToEditEffect) {
            _hitRuleListOwner = _stanceToEditPerk ? &_stanceToEditPerk->hitRules : &_stanceToEditEffect->hitRules;
        } else if (_movesetToEditPerk || _movesetToEditEffect) {
            _hitRuleListOwner = _movesetToEditPerk ? &_movesetToEditPerk->hitRules : &_movesetToEditEffect->hitRules;
        } else if (_subMovesetToEditPerk || _subMovesetToEditEffect) {
            _hitRuleListOwner =
                _subMovesetToEditPerk ? &_subMovesetToEditPerk->hitRules : &_subMovesetToEditEffect->hitRules;
        } else if (!_editingPlayer2HPerks && !_editingNPC2HPerks && !_editingNPCDual2HPerks) {
            _hitRuleListOwner = nullptr;
        }
    }

    if (_isConditionsEffectsPopupOpen) {
        ImGui::OpenPopup("Conditions & Effects");
    }


    static std::vector<PerkDef> tempSelectedPerks;
    static std::vector<AppliedEffect> tempSelectedEffects;

    // Configuração do popup (tamanho, posição)
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 center = ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x * 0.6f, viewport->Size.y * 0.7f));

    if (ImGui::BeginPopupModal("Conditions & Effects", &_isConditionsEffectsPopupOpen, ImGuiWindowFlags_None)) {

        static int perkDisplayFilter = 0;


        bool isEditing2HConfig = _editingPlayer2HPerks || _editingNPC2HPerks || _editingNPCDual2HPerks;
        bool isEditingHitRule = (_hitRuleToEdit != nullptr);

        // Carrega/Prepara os dados quando o popup aparece
        if (ImGui::IsWindowAppearing()) {
            // Perks: Limpa e preenche _perksToDisplayInPopup e _inheritedPerkFormIDs
            // (Esta lógica precisa ser movida para onde o botão é clicado, veja passo 4)
            // Remove duplicatas de _perksToDisplayInPopup
            std::sort(_perksToDisplayInPopup.begin(), _perksToDisplayInPopup.end(),
                      [](const PerkDef& a, const PerkDef& b) { return a.formID < b.formID; });
            _perksToDisplayInPopup.erase(
                std::unique(_perksToDisplayInPopup.begin(), _perksToDisplayInPopup.end(),
                            [](const PerkDef& a, const PerkDef& b) { return a.formID == b.formID; }),
                _perksToDisplayInPopup.end());
            tempSelectedPerks = _perksToDisplayInPopup;  // Copia para a lista temporária da UI

            // Effects: Limpa e preenche _effectsToDisplayInPopup e _inheritedEffectFormIDs
            // (Esta lógica precisa ser movida para onde o botão é clicado, veja passo 4)
            // Remove duplicatas de _effectsToDisplayInPopup
            std::sort(_effectsToDisplayInPopup.begin(), _effectsToDisplayInPopup.end());
            _effectsToDisplayInPopup.erase(
                std::unique(_effectsToDisplayInPopup.begin(), _effectsToDisplayInPopup.end()),
                _effectsToDisplayInPopup.end());
            tempSelectedEffects = _effectsToDisplayInPopup;  // Copia para a lista temporária da UI

            if (isEditing2HConfig) {
                _effectsToDisplayInPopup.clear();
                _inheritedEffectFormIDs.clear();
                tempSelectedEffects.clear();
                SKSE::log::info("[DrawConditionsEffectsPopup] Editando config 2H, listas de efeitos limpas.");
            }


            strcpy_s(_perkFilter, "");
            strcpy_s(_effectFilter, "");
            _effectTypeFilter = 0;
            perkDisplayFilter = 0;

        }

        // Cria as abas
        if (ImGui::BeginTabBar("ConditionsEffectsTabs")) {
            // --- Aba de Required Perks ---
            if (ImGui::BeginTabItem("Required Perks")) {                             // Use LOC(...)
                ImGui::InputText("Filter Perks", _perkFilter, sizeof(_perkFilter));  // Use LOC(...)


                ImGui::SameLine();
                const char* perkFilters[] = {"All", "Selected"};
                ImGui::PushItemWidth(150);
                ImGui::Combo("Filter By", &perkDisplayFilter, perkFilters, 2);
                ImGui::PopItemWidth();


                ImGui::Separator();

                // Lista de Perks (conteúdo de DrawPerkSelectorPopup - parte da lista)
                if (ImGui::BeginChild("PerkListChild", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2))) {
                    std::string filter_str = _perkFilter;
                    std::transform(filter_str.begin(), filter_str.end(), filter_str.begin(), ::tolower);


                    if (perkDisplayFilter == 0) {  // "All"
                        for (const auto& perk : _allPerks) {
                            std::string perk_name_lower = perk.name;
                            std::transform(perk_name_lower.begin(), perk_name_lower.end(), perk_name_lower.begin(),
                                           ::tolower);

                            if (filter_str.empty() || perk_name_lower.find(filter_str) != std::string::npos) {
                                auto it = std::find_if(tempSelectedPerks.begin(), tempSelectedPerks.end(),
                                                       [&](const PerkDef& p) { return p.formID == perk.formID; });
                                bool is_selected = (it != tempSelectedPerks.end());
                                bool is_inherited = _inheritedPerkFormIDs.count(perk.formID);

                                if (is_inherited) ImGui::BeginDisabled();

                                std::string unique_label = std::format("{}##Perk_{:X}", perk.name, perk.formID);
                                if (ImGui::Checkbox(unique_label.c_str(), &is_selected)) {
                                    if (!is_inherited) {  // Só permite alterar se não for herdado
                                        if (is_selected) {
                                            std::string origin = "";  // A origem será definida ao salvar
                                            tempSelectedPerks.push_back({perk.pluginName, perk.formID, origin});
                                        } else {
                                            auto to_erase =
                                                std::find_if(tempSelectedPerks.begin(), tempSelectedPerks.end(),
                                                             [&](const PerkDef& p) { return p.formID == perk.formID; });
                                            if (to_erase != tempSelectedPerks.end()) {
                                                tempSelectedPerks.erase(to_erase);
                                            }
                                        }
                                    }
                                }

                                if (is_inherited) ImGui::EndDisabled();

                                ImGui::SameLine(350);
                                ImGui::TextDisabled("%s | %s", perk.editorID.c_str(), perk.pluginName.c_str());
                            }
                        }
                    } else {  // "Selected"
                        for (auto it = tempSelectedPerks.begin(); it != tempSelectedPerks.end(); /* no increment */) {
                            const auto& selectedPerkDef = *it;

                            // Encontra o PerkInfo original para exibir os detalhes
                            const PerkInfo* perk = nullptr;
                            for (const auto& pInfo : _allPerks) {
                                if (pInfo.formID == selectedPerkDef.formID) {
                                    perk = &pInfo;
                                    break;
                                }
                            }
                            if (!perk) {
                                ++it;  // Perk não encontrado na lista mestre, pula
                                continue;
                            }

                            std::string perk_name_lower = perk->name;
                            std::transform(perk_name_lower.begin(), perk_name_lower.end(), perk_name_lower.begin(),
                                           ::tolower);

                            if (filter_str.empty() || perk_name_lower.find(filter_str) != std::string::npos) {
                                bool is_selected = true;  // Estamos iterando a lista de selecionados
                                bool is_inherited = _inheritedPerkFormIDs.count(perk->formID);

                                if (is_inherited) ImGui::BeginDisabled();

                                std::string unique_label = std::format("{}##Perk_{:X}", perk->name, perk->formID);
                                if (ImGui::Checkbox(unique_label.c_str(), &is_selected)) {
                                    if (!is_inherited) {
                                        if (!is_selected) {  // Está sendo desmarcado
                                            // Apaga da lista temporária e atualiza o iterador
                                            it = tempSelectedPerks.erase(it);
                                            continue;  // Pula o resto do loop e o incremento
                                        }
                                    }
                                }

                                if (is_inherited) ImGui::EndDisabled();

                                ImGui::SameLine(350);
                                ImGui::TextDisabled("%s | %s", perk->editorID.c_str(), perk->pluginName.c_str());
                            }
                            ++it;  // Incrementa o iterador
                        }
                    }
                    // --- FIM DA CORREÇÃO ---
                }
                ImGui::EndChild();
                ImGui::Separator();

                if (ImGui::Button(LOC("clear_all"), ImVec2(120, 0))) {
                    // Mantém apenas os herdados na lista temporária
                    tempSelectedPerks.erase(std::remove_if(tempSelectedPerks.begin(), tempSelectedPerks.end(),
                                                           [&](const PerkDef& p) {
                                                               return _inheritedPerkFormIDs.find(p.formID) ==
                                                                      _inheritedPerkFormIDs.end();
                                                           }),
                                            tempSelectedPerks.end());
                }

                ImGui::EndTabItem();
            }
            if (!isEditing2HConfig) {
                // --- Aba de Apply Effects ---
                if (ImGui::BeginTabItem("Apply Effects")) {  // Use LOC(...)
                                                             // Filtros (conteúdo de DrawEffectSelectorPopup)
                    ImGui::InputText("Filter Name/EditorID", _effectFilter, sizeof(_effectFilter));  // Use LOC(...)
                    ImGui::SameLine();

                    // --- INÍCIO DA CORREÇÃO: Adiciona "Selected" ao combo ---
                    const char* types[] = {"All Types", "Perks", "Spells", "Selected"};
                    ImGui::PushItemWidth(150);
                    ImGui::Combo("Filter Type", &_effectTypeFilter, types, 4);  // Alterado de 3 para 4
                    ImGui::PopItemWidth();
                    // --- FIM DA CORREÇÃO ---

                    ImGui::Separator();

                    // Lista de Efeitos (conteúdo de DrawEffectSelectorPopup - parte da lista)
                    if (ImGui::BeginChild("EffectListChild", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2))) {
                        std::string filter_str = _effectFilter;
                        std::transform(filter_str.begin(), filter_str.end(), filter_str.begin(), ::tolower);

                        // --- INÍCIO DA CORREÇÃO: Lógica draw_list removida do stable_partition ---
                        auto draw_list = [&](const auto& sourceList, AppliedEffect::EffectType type,
                                             const char* typeLabel) {
                            for (const auto& item : sourceList) {
                                std::string name_lower = item.name;
                                std::string editorid_lower = item.editorID;
                                std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
                                std::transform(editorid_lower.begin(), editorid_lower.end(), editorid_lower.begin(),
                                               ::tolower);

                                if (filter_str.empty() || name_lower.find(filter_str) != std::string::npos ||
                                    editorid_lower.find(filter_str) != std::string::npos) {
                                    auto it = std::find_if(tempSelectedEffects.begin(), tempSelectedEffects.end(),
                                                           [&](const AppliedEffect& ae) {
                                                               return ae.formID == item.formID && ae.type == type;
                                                           });
                                    bool is_selected = (it != tempSelectedEffects.end());
                                    bool is_inherited = _inheritedEffectFormIDs.count(item.formID);

                                    if (is_inherited) ImGui::BeginDisabled();

                                    std::string unique_label =
                                        std::format("{}##{}_{:X}", item.name, typeLabel, item.formID);
                                    if (ImGui::Checkbox(unique_label.c_str(), &is_selected)) {
                                        if (!is_inherited) {  // Só permite alterar se não for herdado
                                            if (is_selected) {
                                                std::string origin = "";  // Origem definida ao salvar
                                                tempSelectedEffects.push_back(
                                                    {type, item.pluginName, item.formID, origin});
                                            } else {
                                                auto to_erase =
                                                    std::find_if(tempSelectedEffects.begin(), tempSelectedEffects.end(),
                                                                 [&](const AppliedEffect& ae) {
                                                                     return ae.formID == item.formID && ae.type == type;
                                                                 });
                                                if (to_erase != tempSelectedEffects.end()) {
                                                    tempSelectedEffects.erase(to_erase);
                                                }
                                            }
                                        }
                                    }

                                    if (is_inherited) ImGui::EndDisabled();

                                    ImGui::SameLine(400);
                                    ImGui::TextDisabled("[%s] %s | %s", typeLabel, item.editorID.c_str(),
                                                        item.pluginName.c_str());
                                }
                            }
                        };

                        if (_effectTypeFilter == 3) {  // "Selected"
                            for (auto it = tempSelectedEffects.begin(); it != tempSelectedEffects.end();
                                 /* no increment */) {
                                const auto& selectedEffect = *it;
                                const char* typeLabel = "Unknown";
                                std::string name, editorID, pluginName;
                                bool found = false;

                                if (selectedEffect.type == AppliedEffect::EffectType::Perk) {
                                    typeLabel = "Perk";
                                    for (const auto& pInfo : _allPerks) {
                                        if (pInfo.formID == selectedEffect.formID) {
                                            name = pInfo.name;
                                            editorID = pInfo.editorID;
                                            pluginName = pInfo.pluginName;
                                            found = true;
                                            break;
                                        }
                                    }
                                } else if (selectedEffect.type == AppliedEffect::EffectType::Spell) {
                                    typeLabel = "Spell";
                                    for (const auto& sInfo : _allSpells) {
                                        if (sInfo.formID == selectedEffect.formID) {
                                            name = sInfo.name;
                                            editorID = sInfo.editorID;
                                            pluginName = sInfo.pluginName;
                                            found = true;
                                            break;
                                        }
                                    }
                                }

                                if (!found) {
                                    ++it;
                                    continue;
                                }  // Item não encontrado na lista mestre

                                std::string name_lower = name;
                                std::string editorid_lower = editorID;
                                std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
                                std::transform(editorid_lower.begin(), editorid_lower.end(), editorid_lower.begin(),
                                               ::tolower);

                                if (filter_str.empty() || name_lower.find(filter_str) != std::string::npos ||
                                    editorid_lower.find(filter_str) != std::string::npos) {
                                    bool is_selected = true;
                                    bool is_inherited = _inheritedEffectFormIDs.count(selectedEffect.formID);

                                    if (is_inherited) ImGui::BeginDisabled();

                                    std::string unique_label =
                                        std::format("{}##{}_{:X}", name, typeLabel, selectedEffect.formID);
                                    if (ImGui::Checkbox(unique_label.c_str(), &is_selected)) {
                                        if (!is_inherited && !is_selected) {
                                            // Desmarcado, apaga e atualiza o iterador
                                            it = tempSelectedEffects.erase(it);
                                            continue;
                                        }
                                    }
                                    if (is_inherited) ImGui::EndDisabled();

                                    ImGui::SameLine(400);
                                    ImGui::TextDisabled("[%s] %s | %s", typeLabel, editorID.c_str(),
                                                        pluginName.c_str());
                                }
                                ++it;
                            }

                        } else {  // "All", "Perks", or "Spells"
                            if (_effectTypeFilter == 0 || _effectTypeFilter == 1)
                                draw_list(_allPerks, AppliedEffect::EffectType::Perk, "Perk");
                            if (_effectTypeFilter == 0 || _effectTypeFilter == 2)
                                draw_list(_allSpells, AppliedEffect::EffectType::Spell, "Spell");
                        }
                        // --- FIM DA CORREÇÃO ---
                    }
                    ImGui::EndChild();
                    ImGui::Separator();

                    if (ImGui::Button(LOC("clear_all"), ImVec2(120, 0))) {
                        // Mantém apenas os herdados na lista temporária
                        tempSelectedEffects.erase(std::remove_if(tempSelectedEffects.begin(), tempSelectedEffects.end(),
                                                                 [&](const AppliedEffect& p) {
                                                                     return _inheritedEffectFormIDs.find(p.formID) ==
                                                                            _inheritedEffectFormIDs.end();
                                                                 }),
                                                  tempSelectedEffects.end());
                    }

                    ImGui::EndTabItem();
                }
            }
            if (!isEditing2HConfig &&
                !isEditingHitRule) {  // Não mostra esta aba se estiver editando 2H ou uma Hit Rule
                if (ImGui::BeginTabItem("Hit Count")) {
                    if (ImGui::Button("Create New Rule...")) {
                        _isConditionsEffectsPopupOpen = false;
                        if (_hitRuleListOwner) {                // Só abre se soubermos onde salvar
                            _hitCountRuleEditorHitNumber = 0;   // Reseta o buffer
                            _isHitCountNumberPopupOpen = true;  // Ativa o popup de número
                            _hitRuleToEdit = nullptr;           // Garante que estamos em modo de criação
                        } else {
                            SKSE::log::error("[HitCountTab] _hitRuleListOwner é nulo. Não é possível criar regra.");
                        }
                    }

                    ImGui::Separator();

                    if (ImGui::BeginTable(
                            "HitRulesTable", 3,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                        ImGui::TableSetupColumn("Hit Count", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                        ImGui::TableSetupColumn("Conditions / Effects", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 180.0f);
                        ImGui::TableHeadersRow();

                        std::sort(_inheritedHitRules.begin(), _inheritedHitRules.end());
                        _inheritedHitRules.erase(std::unique(_inheritedHitRules.begin(), _inheritedHitRules.end(),
                                                             [](const HitCountRule& a, const HitCountRule& b) {
                                                                 return a.hitCount == b.hitCount;
                                                             }),
                                                 _inheritedHitRules.end());

                        for (const auto& rule : _inheritedHitRules) {
                            ImGui::PushID(&rule);  // ID único para a regra herdada
                            ImGui::TableNextRow();
                            ImGui::BeginDisabled();  // Desabilita a linha inteira

                            ImGui::TableNextColumn();
                            ImGui::Text("%d", rule.hitCount);

                            ImGui::TableNextColumn();
                            ImGui::Text("Perks: %zu / Effects: %zu", rule.perks.size(), rule.effects.size());
                            ImGui::SameLine();
                            ImGui::TextDisabled(" (Inherited)");  // Indica que é herdada

                            ImGui::TableNextColumn();
                            ImGui::Button("Edit");  // Botões falsos desabilitados
                            ImGui::SameLine();
                            ImGui::Button("Delete");

                            ImGui::EndDisabled();  // Fim da desabilitação
                            ImGui::PopID();
                        }

                        // 2. Renderizar Regras Próprias (Habilitadas)
                        if (_hitRuleListOwner) {
                            for (auto it = _hitRuleListOwner->begin(); it != _hitRuleListOwner->end();) {
                                auto& rule = *it;

                                bool isOverridden = false;
                                for (const auto& inheritedRule : _inheritedHitRules) {
                                    if (inheritedRule.hitCount == rule.hitCount) {
                                        isOverridden = true;
                                        break;
                                    }
                                }

                                ImGui::PushID(&rule);
                                ImGui::TableNextRow();

                                ImGui::TableNextColumn();
                                ImGui::Text("%d", rule.hitCount);
                                if (isOverridden) {
                                    ImGui::SameLine();
                                    ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.0f, 1.0f), " (Overrides)");
                                }

                                ImGui::TableNextColumn();
                                ImGui::Text("Perks: %zu / Effects: %zu", rule.perks.size(), rule.effects.size());

                                ImGui::TableNextColumn();
                                if (ImGui::Button("Edit")) {
                                    _hitRuleToEdit = &rule;
                                    _perksToDisplayInPopup = rule.perks;
                                    _effectsToDisplayInPopup = rule.effects;
                                    _inheritedPerkFormIDs.clear();
                                    _inheritedEffectFormIDs.clear();
                                    _inheritedHitRules.clear();

                                    _isConditionsEffectsPopupOpen = true;
                                    ImGui::CloseCurrentPopup();
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Delete")) {
                                    it = _hitRuleListOwner->erase(it);
                                } else {
                                    ++it;
                                }

                                ImGui::PopID();
                            }
                        }

                        ImGui::EndTable();
                    }

                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        if (ImGui::Button(LOC("save"), ImVec2(120, 0))) {
            std::vector<AppliedEffect> effectsToSave;
            for (const auto& effect : tempSelectedEffects) {
                bool isInherited = _inheritedEffectFormIDs.count(effect.formID);
                if (!isInherited) {                      // Salva apenas os NÃO herdados
                    std::string origin = effect.origin;  // Mantem a origem se já existir
                    if (origin.empty()) {
                        if (_hitRuleToEdit) origin = "HitRule";
                        if (_stanceToEditEffect)
                            origin = "Stance";  // Usa ponteiro ...Effect
                        else if (_movesetToEditEffect)
                            origin = "Moveset";  // Usa ponteiro ...Effect
                        else if (_subMovesetToEditEffect)
                            origin = "SubMoveset";  // Usa ponteiro ...Effect
                    }
                    effectsToSave.push_back({effect.type, effect.pluginName, effect.formID, origin});
                }
            }
            if (_hitRuleToEdit) 
                _hitRuleToEdit->effects = effectsToSave;
            else if (_stanceToEditEffect)
                _stanceToEditEffect->appliedEffects = effectsToSave;
            else if (_movesetToEditEffect)
                _movesetToEditEffect->appliedEffects = effectsToSave;
            else if (_subMovesetToEditEffect)
                _subMovesetToEditEffect->appliedEffects = effectsToSave;

            std::vector<PerkDef> perksToSave;
            for (const auto& perk : tempSelectedPerks) {
                bool isInherited = _inheritedPerkFormIDs.count(perk.formID);
                // Salva apenas se NÃO for herdado OU se estiver editando configs 2H
                if (!isInherited) {
                    std::string origin = perk.origin;  // Mantem a origem se já existir
                    // Define a origem correta se for uma nova adição ou config 2H
                    if (origin.empty() || _editingPlayer2HPerks || _editingNPC2HPerks || _editingNPCDual2HPerks) {
                        if (_hitRuleToEdit)  
                            origin = "HitRule";
                        else if (_editingPlayer2HPerks)
                            origin = "Player2H";
                        else if (_editingNPC2HPerks)
                            origin = "NPC2H";
                        else if (_editingNPCDual2HPerks)
                            origin = "NPCDual2H";
                        else if (_stanceToEditPerk)
                            origin = "Stance";  // Usa ponteiro ...Perk
                        else if (_movesetToEditPerk)
                            origin = "Moveset";  // Usa ponteiro ...Perk
                        else if (_subMovesetToEditPerk)
                            origin = "SubMoveset";  // Usa ponteiro ...Perk
                    }
                    perksToSave.push_back({perk.pluginName, perk.formID, origin});
                }
            }

            // Salva na struct apropriada usando os ponteiros ...Perk
            if (_hitRuleToEdit)  
                _hitRuleToEdit->perks = perksToSave;
            else if (_editingPlayer2HPerks)
                handle::player2HConfig.requiredPerks = perksToSave;
            else if (_editingNPC2HPerks)
                handle::npc2HConfig.requiredPerks = perksToSave;
            else if (_editingNPCDual2HPerks)
                handle::npc2HConfig.requiredPerksDual2H = perksToSave;
            else if (_stanceToEditPerk)
                _stanceToEditPerk->perkList = perksToSave;
            else if (_movesetToEditPerk)
                _movesetToEditPerk->perkList = perksToSave;
            else if (_subMovesetToEditPerk)
                _subMovesetToEditPerk->perkList = perksToSave;
            //_hitRuleToEdit = nullptr;
            //ImGui::CloseCurrentPopup();  // Fecha o popup inteiro
        }
        ImGui::EndPopup();

    } else {

        _editingPlayer2HPerks = false;
        _editingNPC2HPerks = false;
        _editingNPCDual2HPerks = false;
        _hitRuleToEdit = nullptr;

    }
}


void AnimationManager::DrawHitCountNumberPopup() {
    if (_isHitCountNumberPopupOpen) {
        ImGui::OpenPopup("Enter Hit Count");
        // Foca o input de texto automaticamente
        ImGui::SetNextWindowFocus();
    }

    // Centraliza o popup
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 center = ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    static std::string hit_count_error_msg; 
    if (ImGui::BeginPopupModal("Enter Hit Count", &_isHitCountNumberPopupOpen, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Enter the number of hits for this rule:");
        ImGui::PushItemWidth(150);
        // Foca o InputInt na primeira vez que o popup é desenhado
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        if (ImGui::InputInt("##HitCountInput", &_hitCountRuleEditorHitNumber, 1, 5)) {  
            
                          
            if (ImGui::Button("Save", ImVec2(120, 0))) {
                if (_hitCountRuleEditorHitNumber <= 0) {
                    hit_count_error_msg = "Hit count must be greater than 0.";
                } else if (!_hitRuleListOwner) {
                    hit_count_error_msg = "Error: Rule owner is not set.";
                } else {
                    // Verifica se já existe uma regra com esse número
                    bool exists = false;
                    for (const auto& rule : *_hitRuleListOwner) {
                        if (rule.hitCount == _hitCountRuleEditorHitNumber) {
                            exists = true;
                            break;
                        }
                    }

                    if (exists) {
                        hit_count_error_msg = "A rule for this hit count already exists.";
                    } else {
                        // --- SUCESSO ---
                        hit_count_error_msg = "";  // Limpa erro

                        HitCountRule newRule;
                        newRule.hitCount = _hitCountRuleEditorHitNumber;

                        _hitRuleListOwner->push_back(newRule);
                        // Mantém a lista ordenada
                        std::sort(_hitRuleListOwner->begin(), _hitRuleListOwner->end());

                        // Encontra o ponteiro para a regra que acabamos de adicionar
                        HitCountRule* newRulePtr = nullptr;
                        for (auto& rule : *_hitRuleListOwner) {
                            if (rule.hitCount == _hitCountRuleEditorHitNumber) {
                                newRulePtr = &rule;
                                break;
                            }
                        }

                        if (newRulePtr) {
                            _hitRuleToEdit = newRulePtr;  // Aponta para a nova regra
                            // Prepara o popup principal para editar a nova regra
                            _perksToDisplayInPopup.clear();
                            _effectsToDisplayInPopup.clear();
                            _inheritedPerkFormIDs.clear();
                            _inheritedEffectFormIDs.clear();

                            _isConditionsEffectsPopupOpen = true;  // Ativa o popup principal
                        }

                        _isHitCountNumberPopupOpen = false;  // Fecha este popup
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
        }
        ImGui::PopItemWidth();

        ImGui::Separator();

        

        if (ImGui::Button("Save", ImVec2(120, 0))) {
            if (_hitCountRuleEditorHitNumber <= 0) {
                hit_count_error_msg = "Hit count must be greater than 0.";
            } else if (!_hitRuleListOwner) {
                hit_count_error_msg = "Error: Rule owner is not set.";
            } else {
                // Verifica se já existe uma regra com esse número
                bool exists = false;
                for (const auto& rule : *_hitRuleListOwner) {
                    if (rule.hitCount == _hitCountRuleEditorHitNumber) {
                        exists = true;
                        break;
                    }
                }

                if (exists) {
                    hit_count_error_msg = "A rule for this hit count already exists.";
                } else {
                    // --- SUCESSO ---
                    hit_count_error_msg = "";  // Limpa erro

                    HitCountRule newRule;
                    newRule.hitCount = _hitCountRuleEditorHitNumber;

                    _hitRuleListOwner->push_back(newRule);
                    // Mantém a lista ordenada
                    std::sort(_hitRuleListOwner->begin(), _hitRuleListOwner->end());

                    // Encontra o ponteiro para a regra que acabamos de adicionar
                    HitCountRule* newRulePtr = nullptr;
                    for (auto& rule : *_hitRuleListOwner) {
                        if (rule.hitCount == _hitCountRuleEditorHitNumber) {
                            newRulePtr = &rule;
                            break;
                        }
                    }

                    if (newRulePtr) {
                        _hitRuleToEdit = newRulePtr;  // Aponta para a nova regra
                        // Prepara o popup principal para editar a nova regra
                        _perksToDisplayInPopup.clear();
                        _effectsToDisplayInPopup.clear();
                        _inheritedPerkFormIDs.clear();
                        _inheritedEffectFormIDs.clear();

                        _isConditionsEffectsPopupOpen = true;  // Ativa o popup principal
                    }

                    _isHitCountNumberPopupOpen = false;  // Fecha este popup
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            hit_count_error_msg = "";  // Limpa erro
            _isHitCountNumberPopupOpen = false;
            ImGui::CloseCurrentPopup();
            // --- INÍCIO DA CORREÇÃO: Reabre o popup principal ---
            _isConditionsEffectsPopupOpen = true;
            // --- FIM DA CORREÇÃO ---
        }

        // Exibe a mensagem de erro, se houver
        if (!hit_count_error_msg.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", hit_count_error_msg.c_str());
        }

        ImGui::EndPopup();
    }
}

