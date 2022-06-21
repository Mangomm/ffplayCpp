#ifndef PROPERTIES_H
#define PROPERTIES_H

#include <string>
#include <string.h>
#include <vector>
#include <map>
#include "ffplaycppdll_global.h"

namespace N1 {namespace N2 { namespace N3 {

class FFPLAYCPPDLLSHARED_EXPORT Properties: public std::map<std::string, std::string>
{
    public:
        Properties();
        ~Properties();

    public:
        bool HasProperty(const std::string &key) const;

        void SetProperty(const char* key, int intval);
        void SetProperty(const char* key, uint32_t val);
        void SetProperty(const char* key, uint64_t val);
        void SetProperty(const char* key, const char* val);
        void SetProperty(const std::string &key, const std::string &val);

        const char* GetProperty(const char* key) const;
        std::string GetProperty(const char* key, const std::string defaultValue) const;
        std::string GetProperty(const std::string &key, const std::string defaultValue) const;
        const char* GetProperty(const char* key, const char *defaultValue) const;
        const char* GetProperty(const std::string &key, char *defaultValue) const;
        int GetProperty(const char* key, int defaultValue) const;
        int GetProperty(const std::string &key, int defaultValue) const;
        uint64_t GetProperty(const char* key, uint64_t defaultValue) const;
        uint64_t GetProperty(const std::string &key, uint64_t defaultValue) const;
        bool GetProperty(const char* key, bool defaultValue) const;
        bool GetProperty(const std::string &key, bool defaultValue) const;

        void GetChildren(const std::string& path, Properties &children) const;
        void GetChildren(const char* path, Properties &children) const;
        Properties GetChildren(const std::string& path) const;
        Properties GetChildren(const char* path) const;

        void GetChildrenArray(const char* path, std::vector<Properties> &array) const;
};


}
}
}

#endif // PROPERTIES_H
