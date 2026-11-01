#include "NetworkTypes.h"

namespace Net {

std::string Response::header(const std::string& name) const
{
    std::string lower;
    lower.reserve(name.size());
    for (size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        lower.push_back(c);
    }

    for (size_t i = 0; i < headers.size(); ++i) {
        std::string key;
        key.reserve(headers[i].first.size());
        for (size_t k = 0; k < headers[i].first.size(); ++k) {
            char c = headers[i].first[k];
            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
            key.push_back(c);
        }
        if (key == lower)
            return headers[i].second;
    }
    return std::string();
}

} /* namespace Net */
