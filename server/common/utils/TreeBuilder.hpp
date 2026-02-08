#pragma once

/**
 * @brief 树形结构构建工具
 */
class TreeBuilder {
public:
    static Json::Value build(const Json::Value& items,
                              const std::string& idField = "id",
                              const std::string& parentField = "parent_id",
                              const std::string& childrenField = "children") {
        if (!items.isArray()) {
            return Json::Value(Json::arrayValue);
        }

        std::unordered_map<int, Json::Value> itemMap;
        for (const auto& item : items) {
            int id = item[idField].asInt();
            itemMap[id] = item;
        }

        Json::Value roots(Json::arrayValue);

        for (auto& [id, item] : itemMap) {
            int parentId = 0;
            if (item.isMember(parentField) && !item[parentField].isNull()) {
                parentId = item[parentField].asInt();
            }

            if (parentId == 0 || itemMap.find(parentId) == itemMap.end()) {
                roots.append(item);
            } else {
                if (!itemMap[parentId].isMember(childrenField)) {
                    itemMap[parentId][childrenField] = Json::Value(Json::arrayValue);
                }
                itemMap[parentId][childrenField].append(item);
            }
        }

        static constexpr int MAX_DEPTH = 20;
        std::function<void(Json::Value&, int)> attachChildren;
        attachChildren = [&itemMap, &idField, &childrenField, &attachChildren](Json::Value& node, int depth) {
            if (depth > MAX_DEPTH) return;
            int id = node[idField].asInt();
            if (itemMap[id].isMember(childrenField)) {
                node[childrenField] = itemMap[id][childrenField];
                for (auto& child : node[childrenField]) {
                    attachChildren(child, depth + 1);
                }
            }
        };

        for (auto& root : roots) {
            attachChildren(root, 0);
        }

        return roots;
    }

    static void sort(Json::Value& tree,
                      const std::string& sortField = "sort_order",
                      bool ascending = true) {
        if (!tree.isArray()) return;

        std::vector<Json::Value> items;
        for (const auto& item : tree) {
            items.push_back(item);
        }

        std::sort(items.begin(), items.end(),
            [&sortField, ascending](const Json::Value& a, const Json::Value& b) {
                int valA = a.isMember(sortField) ? a[sortField].asInt() : 0;
                int valB = b.isMember(sortField) ? b[sortField].asInt() : 0;
                return ascending ? (valA < valB) : (valA > valB);
            });

        tree = Json::Value(Json::arrayValue);
        for (auto& item : items) {
            if (item.isMember("children") && item["children"].isArray()) {
                sort(item["children"], sortField, ascending);
            }
            tree.append(item);
        }
    }

    static Json::Value filter(const Json::Value& tree,
                               std::function<bool(const Json::Value&)> predicate) {
        Json::Value result(Json::arrayValue);

        if (!tree.isArray()) return result;

        for (const auto& node : tree) {
            if (predicate(node)) {
                Json::Value newNode = node;
                if (node.isMember("children") && node["children"].isArray()) {
                    newNode["children"] = filter(node["children"], predicate);
                }
                result.append(newNode);
            }
        }

        return result;
    }

    static std::vector<int> getLeafIds(const Json::Value& tree,
                                         const std::string& idField = "id") {
        std::vector<int> result;

        if (!tree.isArray()) return result;

        for (const auto& node : tree) {
            if (!node.isMember("children") ||
                !node["children"].isArray() ||
                node["children"].empty()) {
                result.push_back(node[idField].asInt());
            } else {
                auto childLeafs = getLeafIds(node["children"], idField);
                result.insert(result.end(), childLeafs.begin(), childLeafs.end());
            }
        }

        return result;
    }

    static std::vector<int> getAllIds(const Json::Value& tree,
                                        const std::string& idField = "id") {
        std::vector<int> result;

        if (!tree.isArray()) return result;

        for (const auto& node : tree) {
            result.push_back(node[idField].asInt());
            if (node.isMember("children") && node["children"].isArray()) {
                auto childIds = getAllIds(node["children"], idField);
                result.insert(result.end(), childIds.begin(), childIds.end());
            }
        }

        return result;
    }
};
