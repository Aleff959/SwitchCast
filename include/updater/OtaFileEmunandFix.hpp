// OtaFileEmunandFix.hpp — Específico para CNX 23.0.1-1 com emuNAND em Arquivos
#pragma once
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <unistd.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace SwitchCast::Updater {

    // Valida se o arquivo baixado no SD é realmente um .nro íntegro que o nx-hbloader aceitará.
    // Suporta tanto NRO simples quanto NRO homebrew com seção ASET (RomFS + Icon + NACP).
    inline bool ValidateNroBinary(const char* path) {
        FILE* fp = std::fopen(path, "rb");
        if (!fp) return false;

        std::fseek(fp, 0, SEEK_END);
        long actualFileSize = std::ftell(fp);
        if (actualFileSize < 0x80) {
            std::fclose(fp);
            return false;
        }

        // No formato NRO:
        // Offset 0x10 = Magic "NRO0" (0x304F524E em little-endian)
        // Offset 0x18 = Tamanho da seção de código executável em uint32_t
        uint32_t magic = 0;
        uint32_t headerSize = 0;

        std::fseek(fp, 0x10, SEEK_SET);
        if (std::fread(&magic, sizeof(uint32_t), 1, fp) != 1) {
            std::fclose(fp);
            return false;
        }

        std::fseek(fp, 0x18, SEEK_SET);
        if (std::fread(&headerSize, sizeof(uint32_t), 1, fp) != 1) {
            std::fclose(fp);
            return false;
        }

        const uint32_t NRO0_MAGIC = 0x304F524E; // "NRO0"
        if (magic != NRO0_MAGIC || headerSize < 0x80 || static_cast<long>(headerSize) > actualFileSize) {
            std::fclose(fp);
            return false;
        }

        // Caso 1: NRO simples puro (tamanho exato do cabeçalho)
        if (static_cast<long>(headerSize) == actualFileSize) {
            std::fclose(fp);
            return true;
        }

        // Caso 2: NRO Homebrew com seção ASET acoplada (Icon, NACP, RomFS)
        // Offset headerSize = Magic "ASET" (0x54455341 em little-endian)
        if (actualFileSize >= static_cast<long>(headerSize + 56)) {
            std::fseek(fp, headerSize, SEEK_SET);
            uint32_t asetMagic = 0;
            if (std::fread(&asetMagic, sizeof(uint32_t), 1, fp) == 1) {
                const uint32_t ASET_MAGIC = 0x54455341; // "ASET"
                if (asetMagic == ASET_MAGIC) {
                    // Estrutura do cabeçalho ASET (56 bytes):
                    // 0x08: icon_offset (u64),  0x10: icon_size (u64)
                    // 0x18: nacp_offset (u64),  0x20: nacp_size (u64)
                    // 0x28: romfs_offset (u64), 0x30: romfs_size (u64)
                    std::fseek(fp, headerSize + 8, SEEK_SET);
                    uint64_t iconOff = 0, iconSz = 0;
                    uint64_t nacpOff = 0, nacpSz = 0;
                    uint64_t romfsOff = 0, romfsSz = 0;
                    if (std::fread(&iconOff, sizeof(uint64_t), 1, fp) == 1 &&
                        std::fread(&iconSz, sizeof(uint64_t), 1, fp) == 1 &&
                        std::fread(&nacpOff, sizeof(uint64_t), 1, fp) == 1 &&
                        std::fread(&nacpSz, sizeof(uint64_t), 1, fp) == 1 &&
                        std::fread(&romfsOff, sizeof(uint64_t), 1, fp) == 1 &&
                        std::fread(&romfsSz, sizeof(uint64_t), 1, fp) == 1) {

                        long expectedTotal = static_cast<long>(headerSize + 56 + iconSz + nacpSz + romfsSz);
                        std::fclose(fp);
                        // Garante que o arquivo bate exatamente com os assets,
                        // rejeitando corrupções HTTP multipart ou arquivos truncados
                        return (expectedTotal == actualFileSize);
                    }
                }
            }
        }

        std::fclose(fp);
        return false;
    }

    // Aplica o OTA respeitando o tempo de flush do cartão SD em emuNAND em arquivos (CNX 23.0.1-1)
    inline bool ApplyOtaForFileEmunand(const char* rawArgv0, const char* tempNroPath) {
#ifdef __SWITCH__
        // 1. Garante que o arquivo baixado não está corrompido nem contém lixo HTTP multipart
        if (!ValidateNroBinary(tempNroPath)) {
            return false;
        }

        // 2. Normaliza o caminho para começar obrigatoriamente com "sdmc:/"
        std::string targetPath = (rawArgv0 && std::strlen(rawArgv0) > 0)
            ? rawArgv0
            : "sdmc:/switch/SwitchCast/SwitchCast.nro";

        if (targetPath.rfind("sdmc:/", 0) != 0) {
            if (targetPath.rfind("sdmc:", 0) == 0) targetPath = "sdmc:/" + targetPath.substr(5);
            else if (targetPath[0] == '/') targetPath = "sdmc:" + targetPath;
            else targetPath = "sdmc:/" + targetPath;
        }

        // 3. Lê o novo .nro validado para a RAM (evita qualquer descritor aberto competindo)
        FILE* src = std::fopen(tempNroPath, "rb");
        if (!src) return false;

        std::fseek(src, 0, SEEK_END);
        size_t size = std::ftell(src);
        std::fseek(src, 0, SEEK_SET);

        uint8_t* buffer = new (std::nothrow) uint8_t[size];
        if (!buffer) {
            std::fclose(src);
            return false;
        }
        size_t readBytes = std::fread(buffer, 1, size, src);
        std::fclose(src);

        if (readBytes != size) {
            delete[] buffer;
            return false;
        }

        // Backup seguro do executável anterior por cópia física direta (sem rename FAT!)
        std::string backupPath = targetPath + ".bak";
        std::remove(backupPath.c_str());
        FILE* oldFp = std::fopen(targetPath.c_str(), "rb");
        if (oldFp) {
            FILE* bakFp = std::fopen(backupPath.c_str(), "wb");
            if (bakFp) {
                char copyBuf[65536];
                size_t cBytes = 0;
                while ((cBytes = std::fread(copyBuf, 1, sizeof(copyBuf), oldFp)) > 0) {
                    std::fwrite(copyBuf, 1, cBytes, bakFp);
                }
                std::fflush(bakFp);
                fsync(fileno(bakFp));
                std::fclose(bakFp);
            }
            std::fclose(oldFp);
            fsdevCommitDevice("sdmc");
        }

        // 4. Apaga o .nro antigo primeiro e comita na FAT32 para zerar o tamanho antigo na emuNAND
        std::remove(targetPath.c_str());
        fsdevCommitDevice("sdmc");
        svcSleepThread(150'000'000ULL); // 150ms para a emuNAND em arquivos liberar a entrada FAT

        // 5. Cria o novo .nro limpo do zero com o tamanho exato do header NRO0
        FILE* dst = std::fopen(targetPath.c_str(), "wb");
        if (!dst) {
            delete[] buffer;
            return false;
        }

        size_t written = std::fwrite(buffer, 1, size, dst);
        std::fflush(dst);
        fsync(fileno(dst));
        std::fclose(dst);
        delete[] buffer;

        std::remove(tempNroPath);

        // 6. CRÍTICO PARA EMUNAND EM ARQUIVOS (CNX):
        // Comita o dispositivo SD e aguarda 500ms para o driver da emuMMC terminar de gravar os blocos
        fsdevCommitDevice("sdmc");
        svcSleepThread(500'000'000ULL);

        if (written != size || !ValidateNroBinary(targetPath.c_str())) {
            return false;
        }

        // 7. Registra o próximo carregamento no nx-hbloader se suportado pelo ambiente
        if (envHasNextLoad()) {
            Result rc = envSetNextLoad(targetPath.c_str(), targetPath.c_str());
            return R_SUCCEEDED(rc);
        }
        return true;
#else
        return true;
#endif
    }

    inline bool ApplyOtaForFileEmunand(const std::string& rawArgv0, const std::string& tempNroPath) {
        return ApplyOtaForFileEmunand(rawArgv0.c_str(), tempNroPath.c_str());
    }

} // namespace SwitchCast::Updater

namespace SwitchOta {
    using SwitchCast::Updater::ValidateNroBinary;
    using SwitchCast::Updater::ApplyOtaForFileEmunand;
} // namespace SwitchOta
