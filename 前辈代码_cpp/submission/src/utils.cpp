#include "utils.hpp"
#include "submission_cli.hpp"
namespace problem2 {
Json read_json_strict(const fs::path &path) {
    std::map<int, std::set<std::string>> keys;
    auto callback = [&](int depth, Json::parse_event_t event, Json &value) {
        if (event == Json::parse_event_t::object_start) keys[depth + 1].clear();
        if (event == Json::parse_event_t::key && !keys[depth].insert(value.get<std::string>()).second)
            throw std::invalid_argument("duplicate JSON key");
        return true;
    };
    return Json::parse(submission::read(path), callback);
}
}
