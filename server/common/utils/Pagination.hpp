#pragma once

/**
 * @brief 分页参数结构
 * page 和 pageSize 都传才分页，任意一个没传则返回全部数据
 */
struct Pagination {
    using HttpRequestPtr = drogon::HttpRequestPtr;
    using HttpResponsePtr = drogon::HttpResponsePtr;
    using HttpResponse = drogon::HttpResponse;
    using enum drogon::HttpStatusCode;

    int page = 1;
    int pageSize = 0;  // 0 表示不分页
    int offset = 0;
    std::string keyword;

    bool isPaged() const { return pageSize > 0; }

    static Pagination fromRequest(const HttpRequestPtr& req) {
        Pagination p;

        auto pageStr = req->getParameter("page");
        auto pageSizeStr = req->getParameter("pageSize");

        // 只有 page 和 pageSize 都传了才分页
        if (!pageStr.empty() && !pageSizeStr.empty()) {
            try {
                p.pageSize = std::stoi(pageSizeStr);
                if (p.pageSize < 1) p.pageSize = 10;
                if (p.pageSize > 100) p.pageSize = 100;
            } catch (...) {
                p.pageSize = 10;
            }

            try {
                p.page = std::stoi(pageStr);
                if (p.page < 1) p.page = 1;
            } catch (...) {
                p.page = 1;
            }

            p.offset = (p.page - 1) * p.pageSize;
        }

        p.keyword = req->getParameter("keyword");
        return p;
    }

    /**
     * @brief 对 Json 数组进行内存分页
     * @return {分页后的数组, 总数}
     */
    static std::pair<Json::Value, int> paginate(const Json::Value& items, const Pagination& page) {
        int total = items.isArray() ? static_cast<int>(items.size()) : 0;

        if (!page.isPaged() || total == 0) {
            return {items, total};
        }

        Json::Value pagedItems(Json::arrayValue);
        int end = std::min(page.offset + page.pageSize, total);
        for (int i = page.offset; i < end; ++i) {
            pagedItems.append(items[i]);
        }
        return {pagedItems, total};
    }

    static HttpResponsePtr buildResponse(const Json::Value& items,
                                          int total,
                                          int page,
                                          int pageSize) {
        Json::Value data;
        data["list"] = items;
        data["total"] = total;

        if (pageSize > 0) {
            data["page"] = page;
            data["pageSize"] = pageSize;
            data["totalPages"] = static_cast<int>(std::ceil(static_cast<double>(total) / pageSize));
        }

        Json::Value json;
        json["code"] = 0;
        json["message"] = "Success";
        json["data"] = data;

        auto resp = HttpResponse::newHttpJsonResponse(json);
        resp->setStatusCode(k200OK);
        return resp;
    }

    std::string limitClause() const {
        if (pageSize <= 0) return "";
        return " LIMIT " + std::to_string(pageSize) + " OFFSET " + std::to_string(offset);
    }
};

/**
 * @brief 查询条件构建器
 */
class QueryBuilder {
private:
    std::vector<std::string> conditions_;
    std::vector<std::string> params_;

public:
    QueryBuilder& eq(const std::string& field, const std::string& value) {
        if (!value.empty()) {
            conditions_.push_back(field + " = ?");
            params_.push_back(value);
        }
        return *this;
    }

    QueryBuilder& like(const std::string& field, const std::string& value) {
        if (!value.empty()) {
            conditions_.push_back(field + " LIKE ?");
            params_.push_back("%" + value + "%");
        }
        return *this;
    }

    QueryBuilder& likeAny(const std::vector<std::string>& fields, const std::string& value) {
        if (!value.empty() && !fields.empty()) {
            std::string condition = "(";
            for (size_t i = 0; i < fields.size(); ++i) {
                if (i > 0) condition += " OR ";
                condition += fields[i] + " LIKE ?";
                params_.push_back("%" + value + "%");
            }
            condition += ")";
            conditions_.push_back(condition);
        }
        return *this;
    }

    QueryBuilder& notDeleted(const std::string& field = "deleted_at") {
        conditions_.push_back(field + " IS NULL");
        return *this;
    }

    std::string whereClause() const {
        if (conditions_.empty()) {
            return "";
        }
        std::string result = " WHERE ";
        for (size_t i = 0; i < conditions_.size(); ++i) {
            if (i > 0) result += " AND ";
            result += conditions_[i];
        }
        return result;
    }

    const std::vector<std::string>& params() const {
        return params_;
    }
};
