#ifndef FCT_BASE_LOCAL_H
#define FCT_BASE_LOCAL_H

#ifndef ENTT_NOEXCEPTION
#define ENTT_NOEXCEPTION 1
#endif

#include "./ThirdParty.h"
#include <cstring>

namespace FCT
{
    /**
     * @brief 区域设置枚举（统一采用UTF-8编码）
     */
    enum class Local
    {
        UTF8,
        zh_CN,
        zh_TW,
        en_US,
        ja_JP,
        ko_KR,

        zh_CN_UTF8 = zh_CN,
        zh_TW_UTF8 = zh_TW,
        en_US_UTF8 = en_US,
        ja_JP_UTF8 = ja_JP,
        ko_KR_UTF8 = ko_KR
    };

    using Locale = Local;

    /**
     * @brief Local映射表项（统一绑定UTF-8与代码页65001）
     */
    struct LocalTableEntry
    {
        Local local;
        const char* shortName;
        const char* localeName;
        const char* fallbackName;
        unsigned int windowsCodePage;
    };

    /**
     * @brief Local配置映射表
     */
    inline constexpr LocalTableEntry kLocalTable[] = {
        { Local::UTF8,  "UTF-8", ".UTF-8",      "C.UTF-8", 65001 },
        { Local::zh_CN, "zh_CN", "zh_CN.UTF-8", ".UTF-8",  65001 },
        { Local::zh_TW, "zh_TW", "zh_TW.UTF-8", ".UTF-8",  65001 },
        { Local::en_US, "en_US", "en_US.UTF-8", ".UTF-8",  65001 },
        { Local::ja_JP, "ja_JP", "ja_JP.UTF-8", ".UTF-8",  65001 },
        { Local::ko_KR, "ko_KR", "ko_KR.UTF-8", ".UTF-8",  65001 },
    };

    inline constexpr const LocalTableEntry& FindLocalTableEntry(Local local) noexcept
    {
        for (const auto& entry : kLocalTable)
        {
            if (entry.local == local)
            {
                return entry;
            }
        }
        return kLocalTable[1];
    }

    inline Local ParseLocalName(const char* name, Local defaultLocal = Local::zh_CN) noexcept
    {
        if (!name || !*name)
        {
            return defaultLocal;
        }
        for (const auto& entry : kLocalTable)
        {
            if (std::strcmp(name, entry.shortName) == 0 || std::strcmp(name, entry.localeName) == 0)
            {
                return entry.local;
            }
        }
        return defaultLocal;
    }

    inline void ApplyConsoleCodePage(unsigned int codePage)
    {
#if defined(_WIN32) || defined(WIN32)
        if (codePage != 0)
        {
            char cmd[32] = {};
            std::snprintf(cmd, sizeof(cmd), "chcp %u", codePage);
            std::system(cmd);
        }
#else
        (void)codePage;
#endif
    }

    inline bool TryApplyStdLocale(const char* localeName)
    {
        if (!localeName)
        {
            return false;
        }
        try
        {
            std::locale loc(localeName);
            std::locale::global(loc);
            std::wcout.imbue(loc);
            std::cout.imbue(loc);
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    inline Local& GetCurrentLocalRef() noexcept
    {
        static Local s_currentLocal = Local::zh_CN;
        return s_currentLocal;
    }

    inline Local GetLocal() noexcept
    {
        return GetCurrentLocalRef();
    }

    inline Local GetLocale() noexcept
    {
        return GetLocal();
    }

    /**
     * @brief 根据Local表设置UTF-8控制台编码与标准库区域设置
     */
    inline bool SetLocal(Local local = Local::zh_CN)
    {
        GetCurrentLocalRef() = local;
        const auto& entry = FindLocalTableEntry(local);
        ApplyConsoleCodePage(entry.windowsCodePage);
#ifndef FCT_ANDROID
        if (TryApplyStdLocale(entry.localeName))
        {
            return true;
        }
        return TryApplyStdLocale(entry.fallbackName);
#else
        return true;
#endif
    }

    inline bool SetLocal(const char* localName)
    {
        return SetLocal(ParseLocalName(localName));
    }

    inline bool SetLocale(Local local = Local::zh_CN)
    {
        return SetLocal(local);
    }

    /**
     * @brief 统一配置UTF-8环境（默认zh_CN.UTF-8）
     */
    inline bool SetupUtf8(Local local = Local::zh_CN)
    {
        return SetLocal(local);
    }
}

#endif // FCT_BASE_LOCAL_H
